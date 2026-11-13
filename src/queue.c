/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* One ordered worker per device. Submission owns copies of the application
 * arrays; command buffers and synchronization objects remain pending until
 * their GPU work and semaphore waits have actually completed. */
#include "zvk_internal.h"

struct zvk_submission {
    struct zvk_submission *next;
    VkSemaphore *waits, *signals;
    VkCommandBuffer *commands, *references;
    uint32_t nwaits, nsignals, ncommands, nreferences;
    VkFence fence;
};
static void submission_free(struct zvk_submission *s)
{
    free(s->waits);
    free(s->signals);
    free(s->commands);
    free(s->references);
    free(s);
}
static VkResult reference(struct zvk_submission *s, VkCommandBuffer command, uint32_t depth)
{
    if (depth > 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (unsigned i = 0; i < s->nreferences; i++)
        if (s->references[i] == command)
            return VK_SUCCESS;
    if (command->state != ZVK_EXECUTABLE &&
        !(command->state == ZVK_PENDING &&
          (command->usage & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)))
        return VK_ERROR_INITIALIZATION_FAILED;
    if ((size_t)s->nreferences + 1 > SIZE_MAX / sizeof *s->references)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkCommandBuffer *refs = realloc(s->references, ((size_t)s->nreferences + 1) * sizeof *refs);
    if (!refs)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->references = refs;
    refs[s->nreferences++] = command;
    for (size_t offset = 0; offset < command->size;) {
        const struct zvk_command *header = (const void *)(command->commands + offset);
        if (header->op == ZVK_EXECUTE) {
            VkCommandBuffer child = *(const VkCommandBuffer *)(header + 1);
            VkResult result = reference(s, child, depth + 1);
            if (result != VK_SUCCESS)
                return result;
        }
        offset += header->size;
    }
    return VK_SUCCESS;
}
static void release(struct zvk_submission *s, int executed, VkResult result)
{
    for (unsigned i = 0; i < s->nreferences; i++) {
        VkCommandBuffer command = s->references[i];
        if (!--command->pending)
            command->state = result != VK_SUCCESS ||
                                     (executed && (command->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))
                                 ? ZVK_INVALID : ZVK_EXECUTABLE;
    }
}
static void *queue_worker(void *opaque)
{
    VkDevice d = opaque;
    pthread_mutex_lock(&d->mutex);
    for (;;) {
        while (!d->submissions && !d->worker_stop)
            pthread_cond_wait(&d->condition, &d->mutex);
        if (!d->submissions)
            break;
        struct zvk_submission *s = d->submissions;
        for (;;) {
            int ready = 1;
            for (unsigned i = 0; i < s->nwaits; i++)
                ready &= s->waits[i]->signaled;
            if (ready || d->lost || d->worker_stop)
                break;
            pthread_cond_wait(&d->condition, &d->mutex);
        }
        VkResult result = d->lost;
        if (result == VK_SUCCESS && !d->worker_stop) {
            for (unsigned i = 0; i < s->nwaits; i++)
                s->waits[i]->signaled = 0;
            for (unsigned i = 0; i < s->ncommands && result == VK_SUCCESS; i++)
                result = zvk_execute(&d->queue, s->commands[i], 0);
            if (result == VK_SUCCESS && virgl_flush_fenced(d->gpu))
                result = VK_ERROR_DEVICE_LOST;
            if (result == VK_SUCCESS) {
                for (unsigned i = 0; i < s->nsignals; i++)
                    s->signals[i]->signaled = 1;
                if (s->fence)
                    s->fence->signaled = 1;
            }
        } else if (result == VK_SUCCESS)
            result = VK_ERROR_DEVICE_LOST;
        if (result != VK_SUCCESS)
            d->lost = VK_ERROR_DEVICE_LOST;
        release(s, 1, result);
        d->submissions = s->next;
        if (!d->submissions)
            d->submission_tail = NULL;
        submission_free(s);
        pthread_cond_broadcast(&d->condition);
    }
    pthread_mutex_unlock(&d->mutex);
    return NULL;
}
VkResult zvk_queue_start(VkDevice d)
{
    if (pthread_create(&d->worker, NULL, queue_worker, d))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->worker_started = 1;
    return VK_SUCCESS;
}
void zvk_queue_finish(VkDevice d)
{
    if (!d->worker_started)
        return;
    pthread_mutex_lock(&d->mutex);
    d->worker_stop = 1;
    if (d->submissions)
        d->lost = VK_ERROR_DEVICE_LOST;
    pthread_cond_broadcast(&d->condition);
    pthread_mutex_unlock(&d->mutex);
    pthread_join(d->worker, NULL);
    d->worker_started = 0;
}
static void *array_copy(const void *data, uint32_t count, size_t element)
{
    if (!count)
        return NULL;
    if ((size_t)count > SIZE_MAX / element)
        return NULL;
    void *copy = malloc((size_t)count * element);
    if (copy)
        memcpy(copy, data, (size_t)count * element);
    return copy;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count,
                                             const VkSubmitInfo *submits, VkFence fence)
{
    VkDevice d = queue->device;
    pthread_mutex_lock(&d->mutex);
    VkResult result = d->lost;
    struct zvk_submission *head = NULL, *tail = NULL;
    for (uint32_t i = 0; i < (count ? count : 1) && result == VK_SUCCESS; i++) {
        struct zvk_submission *s = calloc(1, sizeof *s);
        if (!s) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        if (count) {
            const VkSubmitInfo *in = submits + i;
            s->nwaits = in->waitSemaphoreCount;
            s->nsignals = in->signalSemaphoreCount;
            s->ncommands = in->commandBufferCount;
            s->waits = array_copy(in->pWaitSemaphores, s->nwaits, sizeof *s->waits);
            s->signals = array_copy(in->pSignalSemaphores, s->nsignals, sizeof *s->signals);
            s->commands = array_copy(in->pCommandBuffers, s->ncommands, sizeof *s->commands);
            if ((s->nwaits && !s->waits) || (s->nsignals && !s->signals) ||
                (s->ncommands && !s->commands))
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
            for (unsigned j = 0; j < s->ncommands && result == VK_SUCCESS; j++) {
                if (s->commands[j]->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
                    result = VK_ERROR_INITIALIZATION_FAILED;
                else
                    result = reference(s, s->commands[j], 0);
            }
        }
        if (result != VK_SUCCESS) {
            submission_free(s);
            break;
        }
        for (unsigned j = 0; j < s->nreferences; j++) {
            s->references[j]->pending++;
            s->references[j]->state = ZVK_PENDING;
        }
        if (tail)
            tail->next = s;
        else
            head = s;
        tail = s;
    }
    if (result == VK_SUCCESS) {
        tail->fence = fence;
        if (d->submission_tail)
            d->submission_tail->next = head;
        else
            d->submissions = head;
        d->submission_tail = tail;
        pthread_cond_broadcast(&d->condition);
    } else
        while (head) {
            struct zvk_submission *next = head->next;
            release(head, 0, VK_SUCCESS);
            submission_free(head);
            head = next;
        }
    pthread_mutex_unlock(&d->mutex);
    return result;
}

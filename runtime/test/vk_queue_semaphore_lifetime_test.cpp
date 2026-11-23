// Queue progress semaphores stay alive through cross-queue and host waits, without a Vulkan driver.
#include "../src/vulkan_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(EXIT_FAILURE); } \
} while (0)

namespace {

using leonida::ReplayResult;
using leonida::VulkanContext;
using leonida::VulkanQueue;
using Wait = std::pair<VkSemaphore, std::uint64_t>;

struct PausedWait {
    std::latch entered{1};
    std::latch resume{1};
};

struct FakeVulkan {
    struct Semaphore {
        std::uint64_t value = 0;
        unsigned destructions = 0;
    };
    struct Submission {
        std::vector<Wait> waits;
        Wait signal;
    };

    std::map<VkSemaphore, Semaphore> semaphores;
    std::map<VkQueue, std::vector<Submission>> pending;
    std::uintptr_t next_handle = 1;
    VkSemaphore last_created = VK_NULL_HANDLE;
    std::vector<Wait> last_waits;
    bool fail_next_submit = false;
    PausedWait* paused_wait = nullptr;

    bool live(VkSemaphore sem) const { return semaphores.at(sem).destructions == 0; }
    bool waited_for(VkSemaphore sem) const {
        return std::ranges::find(last_waits, Wait{sem, 1}) != last_waits.end();
    }

    static FakeVulkan* active;
    FakeVulkan() { active = this; }
    ~FakeVulkan() {
        for (const auto& entry : semaphores) CHECK(entry.second.destructions == 1);
        active = nullptr;
    }
};

FakeVulkan* FakeVulkan::active = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL fake_create_semaphore(
    VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out) {
    auto& vk = *FakeVulkan::active;
    *out = vk.last_created = reinterpret_cast<VkSemaphore>(vk.next_handle++);
    vk.semaphores.try_emplace(*out);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_semaphore(VkDevice, VkSemaphore sem, const VkAllocationCallbacks*) {
    auto& vk = *FakeVulkan::active;
    CHECK(vk.live(sem));
    for (const auto& entry : vk.pending)
        for (const auto& submit : entry.second) {
            CHECK(submit.signal.first != sem);
            for (const auto& wait : submit.waits) CHECK(wait.first != sem);
        }
    ++vk.semaphores.at(sem).destructions;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_get_counter(VkDevice, VkSemaphore sem, std::uint64_t* value) {
    const auto& vk = *FakeVulkan::active;
    CHECK(vk.live(sem));
    *value = vk.semaphores.at(sem).value;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_wait_semaphores(VkDevice, const VkSemaphoreWaitInfo* info, std::uint64_t) {
    auto& vk = *FakeVulkan::active;
    CHECK(info->semaphoreCount == 1);
    CHECK(vk.live(info->pSemaphores[0]));
    CHECK(vk.paused_wait);
    vk.paused_wait->entered.count_down();
    vk.paused_wait->resume.wait();
    const auto sem = info->pSemaphores[0];
    CHECK(vk.live(sem));
    CHECK(vk.semaphores.at(sem).value >= info->pValues[0]);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_queue_submit(
    VkQueue queue, std::uint32_t count, const VkSubmitInfo* info, VkFence) {
    auto& vk = *FakeVulkan::active;
    CHECK(count == 1 && info->signalSemaphoreCount == 1);
    const auto& timeline = *static_cast<const VkTimelineSemaphoreSubmitInfo*>(info->pNext);
    CHECK(timeline.waitSemaphoreValueCount == info->waitSemaphoreCount);
    vk.last_waits.clear();
    for (std::uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        CHECK(vk.live(info->pWaitSemaphores[i]));
        vk.last_waits.emplace_back(info->pWaitSemaphores[i], timeline.pWaitSemaphoreValues[i]);
    }
    CHECK(vk.live(info->pSignalSemaphores[0]));
    if (std::exchange(vk.fail_next_submit, false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vk.pending[queue].push_back({vk.last_waits, {info->pSignalSemaphores[0], timeline.pSignalSemaphoreValues[0]}});
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_queue_wait_idle(VkQueue queue) {
    auto& vk = *FakeVulkan::active;
    for (const auto& submit : vk.pending[queue]) {
        for (const auto& [sem, value] : submit.waits) {
            CHECK(vk.live(sem));
            CHECK(vk.semaphores.at(sem).value >= value);
        }
        const auto [sem, value] = submit.signal;
        CHECK(vk.live(sem));
        vk.semaphores.at(sem).value = value;
    }
    vk.pending[queue].clear();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_pool(
    VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* out) {
    *out = reinterpret_cast<VkCommandPool>(FakeVulkan::active->next_handle++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_pool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_allocate_commands(
    VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    *out = reinterpret_cast<VkCommandBuffer>(FakeVulkan::active->next_handle++);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_reset_command(VkCommandBuffer, VkCommandBufferResetFlags) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_begin_command(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_end_command(VkCommandBuffer) { return VK_SUCCESS; }

VKAPI_ATTR void VKAPI_CALL fake_fill_buffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, std::uint32_t) {}

VKAPI_ATTR void VKAPI_CALL fake_pipeline_barrier(
    VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t,
    const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_proc(VkDevice, const char* name) {
#define PROC(vk_name, stub) if (std::strcmp(name, #vk_name) == 0) return reinterpret_cast<PFN_vkVoidFunction>(stub)
    PROC(vkCreateSemaphore, fake_create_semaphore);
    PROC(vkDestroySemaphore, fake_destroy_semaphore);
    PROC(vkGetSemaphoreCounterValue, fake_get_counter);
    PROC(vkWaitSemaphores, fake_wait_semaphores);
    PROC(vkQueueSubmit, fake_queue_submit);
    PROC(vkQueueWaitIdle, fake_queue_wait_idle);
    PROC(vkCreateCommandPool, fake_create_pool);
    PROC(vkDestroyCommandPool, fake_destroy_pool);
    PROC(vkAllocateCommandBuffers, fake_allocate_commands);
    PROC(vkResetCommandBuffer, fake_reset_command);
    PROC(vkBeginCommandBuffer, fake_begin_command);
    PROC(vkEndCommandBuffer, fake_end_command);
    PROC(vkCmdFillBuffer, fake_fill_buffer);
    PROC(vkCmdPipelineBarrier, fake_pipeline_barrier);
#undef PROC
    return nullptr;
}

struct Fixture {
    FakeVulkan vk;
    VulkanContext context{fake_get_proc, reinterpret_cast<VkDevice>(1), true};
    bool upload_bindless = false;

    Fixture() {
        context.set_bindless_prepare([this](VkCommandBuffer) {
            if (std::exchange(upload_bindless, false)) context.note_bindless_upload();
            return true;
        });
    }

    std::unique_ptr<VulkanQueue> queue() {
        auto result = std::make_unique<VulkanQueue>(context, reinterpret_cast<VkQueue>(vk.next_handle++), 0);
        CHECK(result->ok());
        return result;
    }

    void publish_storage(VulkanQueue& queue) {
        context.zero_on_next_submit(1, reinterpret_cast<VkBuffer>(1));
        CHECK(queue.execute({}) == ReplayResult::success);
    }

    void retire(VulkanQueue& queue) {
        CHECK(queue.idle());
        queue.marker(leonida::DebugMarker::Kind::set, "retire completed pins");
        context.completed_retire_ticket();
    }
};

void late_bindless_consumption() {
    Fixture f;
    auto producer = f.queue();
    const auto progress = f.vk.last_created;
    f.upload_bindless = true;
    CHECK(producer->execute({}) == ReplayResult::success);
    producer.reset();
    CHECK(f.context.completed_retire_ticket() == 1);

    auto consumer = f.queue();
    CHECK(consumer->execute({}) == ReplayResult::success);
    CHECK(f.vk.waited_for(progress));
    f.retire(*consumer);
    CHECK(f.vk.live(progress));  // The published upload remains available to new queues.
}

void failed_submit_retry() {
    Fixture f;
    auto producer = f.queue();
    const auto progress = f.vk.last_created;
    auto consumer = f.queue();
    auto replacement = f.queue();
    f.publish_storage(*producer);

    f.vk.fail_next_submit = true;
    CHECK(consumer->execute({}) == ReplayResult::unsupported);
    CHECK(f.vk.waited_for(progress));
    f.publish_storage(*replacement);
    producer.reset();
    f.retire(*replacement);
    CHECK(f.vk.live(progress));  // Only the failed submission's queued wait can still own it.

    CHECK(consumer->execute({}) == ReplayResult::success);
    CHECK(f.vk.waited_for(progress));
    CHECK(f.vk.live(progress));
    f.retire(*consumer);
    CHECK(f.vk.semaphores.at(progress).destructions == 1);
}

void unfinished_consumer_submission() {
    Fixture f;
    auto producer = f.queue();
    const auto progress = f.vk.last_created;
    auto consumer = f.queue();
    auto replacement = f.queue();
    f.upload_bindless = true;
    CHECK(producer->execute({}) == ReplayResult::success);
    CHECK(consumer->execute({}) == ReplayResult::success);
    CHECK(f.vk.waited_for(progress));

    f.upload_bindless = true;
    CHECK(replacement->execute({}) == ReplayResult::success);
    producer.reset();
    f.retire(*replacement);
    CHECK(f.vk.live(progress));  // The consumer's GPU wait still names the old upload.
    f.retire(*consumer);
    CHECK(f.vk.semaphores.at(progress).destructions == 1);
}

void paused_host_wait() {
    Fixture f;
    auto producer = f.queue();
    const auto progress = f.vk.last_created;
    CHECK(producer->execute({}) == ReplayResult::success);
    const auto ticket = f.context.submitted_retire_ticket();

    PausedWait pause;
    f.vk.paused_wait = &pause;
    std::uint64_t completed = 0;
    std::thread waiter([&] { completed = f.context.wait_retire_ticket(ticket); });
    pause.entered.wait();
    producer.reset();
    CHECK(f.context.completed_retire_ticket() == ticket);
    CHECK(f.vk.live(progress));  // The host wait snapshot is the last owner.
    pause.resume.count_down();
    waiter.join();
    CHECK(completed == ticket);
    CHECK(f.vk.semaphores.at(progress).destructions == 1);
}

}  // namespace

int main() {
    late_bindless_consumption();
    failed_submit_retry();
    unfinished_consumer_submission();
    paused_host_wait();
}

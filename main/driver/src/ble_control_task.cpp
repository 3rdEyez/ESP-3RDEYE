#include "sdkconfig.h"
#if CONFIG_SATORI_TRANSPORT_BLE_PRIMARY
#include "ble_server_internal.hpp"
#include "servo_group.h"
#include "servor_input_adapter.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "services/gap/ble_svc_gap.h"
#include <cstdio>

namespace satori::ble::internal {
void ControlTask(void*) {
    TickType_t last = xTaskGetTickCount();
    std::uint32_t motion_generation = 0;
    std::uint32_t last_state_event = 0;
    bool servo_enabled = false;
    bool output_fault = false;
    const auto fail_output = [&]() {
        RecordFault(PwmFault, StopReason::OutputFault);
        output_fault = true;
        g_runtime.output_fault = true;
        servo_enabled = false;
        g_runtime.startup_configured = false; // Latched until reboot and configuration revalidation.
        portENTER_CRITICAL(&g_runtime.session_lock);
        g_runtime.session.Disconnect();
        const auto stopped_generation = g_runtime.session.generation();
        portEXIT_CRITICAL(&g_runtime.session_lock);
        g_runtime.motion.Halt(stopped_generation);
        motion_generation = stopped_generation;
        const auto connection = g_runtime.connection.load(std::memory_order_acquire);
        if (connection != kNoConnection) {
            (void)ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        }
        ESP_LOGE(kTag, "PWM driver fault; output commands disabled until reboot");
    };

    const auto state_now = []() {
        Snapshot state;
        portENTER_CRITICAL(&g_runtime.session_lock);
        state = g_runtime.session.snapshot();
        portEXIT_CRITICAL(&g_runtime.session_lock);
        return state;
    };
    const auto valid_work = [](const Outcome& op) {
        portENTER_CRITICAL(&g_runtime.session_lock);
        const bool valid = g_runtime.session.connected() &&
            op.generation == g_runtime.session.generation() &&
            op.request.token != 0 && op.request.token == g_runtime.session.snapshot().token;
        portEXIT_CRITICAL(&g_runtime.session_lock);
        return valid;
    };
    const auto write_output = [&]() {
        // Only this task touches MotionEngine or ServoGroup. No output is
        // initialized by pairing, subscribing, claiming or a GAP callback.
        const Target logical{g_runtime.motion.issued(), 0};
        const auto angles = LogicalTargetToAngles(logical);
        auto& servos = ServoGroup::GetInstance();
        if (!servos.IsReady()) return false;
        for (int channel = 0; channel < 3; ++channel) {
            if (!servos.SetAngle(channel, angles.value[channel])) return false;
        }
        return true;
    };
    const auto publish_motion = [&](std::uint32_t generation, std::uint32_t sequence) {
        portENTER_CRITICAL(&g_runtime.session_lock);
        if (!output_fault && generation == g_runtime.session.generation() && g_runtime.motion.valid_mask() == 7) {
            g_runtime.session.UpdateMotion(g_runtime.motion.issued(), g_runtime.motion.interpolating_mask(), sequence);
        }
        portEXIT_CRITICAL(&g_runtime.session_lock);
    };

    while (true) {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        const auto connection = g_runtime.connection.load(std::memory_order_acquire);
        if (g_runtime.pairing_storage_fault.load(std::memory_order_acquire) && connection != kNoConnection) {
            g_runtime.secure_peer = false;
            portENTER_CRITICAL(&g_runtime.session_lock);
            g_runtime.session.Disconnect();
            const auto fault_generation = g_runtime.session.generation();
            portEXIT_CRITICAL(&g_runtime.session_lock);
            if (g_runtime.work_queue) {
                xQueueReset(g_runtime.work_queue);
                WorkItem stop{}; stop.disconnected = true; stop.outcome.generation = fault_generation;
                (void)xQueueSendToFront(g_runtime.work_queue, &stop, 0);
            }
            (void)ble_gap_terminate(connection, BLE_ERR_AUTH_FAIL);
        }
        std::uint32_t generation;
        bool expired;
        bool had_owner;
        portENTER_CRITICAL(&g_runtime.session_lock);
        had_owner = g_runtime.session.snapshot().token != 0;
        expired = g_runtime.session.TickLease(now);
        generation = g_runtime.session.generation();
        portEXIT_CRITICAL(&g_runtime.session_lock);
        if (motion_generation != generation) {
            g_runtime.motion.Halt(generation);
            motion_generation = generation;
            publish_motion(generation, state_now().last_applied_sequence);
        }
        if (expired && connection != kNoConnection) {
            // RELEASE's ACK window also expires here; it is not a lease loss.
            if (had_owner) RecordStop(StopReason::LeaseExpired);
            (void)ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (connection != kNoConnection &&
            static_cast<std::uint32_t>(now - g_runtime.connected_at_ms) >= 60000) {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(connection, &desc) != 0 || !AuthorizedSecurePeer(desc)) {
                (void)ble_gap_terminate(connection, BLE_ERR_AUTH_FAIL);
            }
        }

        WorkItem item{};
        Outcome latest_target{};
        bool has_target = false;
        // Bounded work per cycle. SET_TARGET is an accepted latest-value slot;
        // stop barriers change generation in the receiver before we get here.
        for (std::size_t count = 0; count < kPendingCommands &&
             xQueueReceive(g_runtime.work_queue, &item, 0) == pdTRUE; ++count) {
            if (item.disconnected) {
                portENTER_CRITICAL(&g_runtime.session_lock);
                generation = g_runtime.session.generation();
                portEXIT_CRITICAL(&g_runtime.session_lock);
                g_runtime.motion.Halt(generation);
                motion_generation = generation;
                has_target = false;
                continue;
            }
            const auto& op = item.outcome;
            if (!valid_work(op)) continue;
            const auto opcode = static_cast<Opcode>(op.request.opcode);
            if (opcode == Opcode::SetTarget) {
                latest_target = op;
                has_target = true;
                continue;
            }
            has_target = false;
            if (opcode == Opcode::SetPairingCode) {
                std::uint32_t requested = 0;
                for (int i = 0; i < 4; ++i) requested |= static_cast<std::uint32_t>(op.request.payload[i]) << (8 * i);
                ble_gap_conn_desc desc{};
                PeerIdentity peer{}; LinkSecurity security{};
                const bool have_desc = ble_gap_conn_find(connection, &desc) == 0;
                if (have_desc) { peer = IdentityFromDesc(desc); security = SecurityFromDesc(desc); }
                char code[7]{}; std::snprintf(code, sizeof(code), "%06lu", static_cast<unsigned long>(requested));
                PairingLock();
                if (!valid_work(op)) { PairingUnlock(); continue; }
                const auto validated = g_runtime.pairing.ValidateCodeChange(peer, security, code);
                esp_err_t persisted = validated == PairingResult::Ok ? BleUpdatePairingCode(requested) : ESP_FAIL;
                const bool code_committed = persisted == ESP_OK && g_runtime.pairing.CommitCodeChange() == PairingResult::Ok;
                if (!code_committed) g_runtime.pairing.AbortCodeChange();
                PairingUnlock();
                if (code_committed) {
                    UpdatePasskey(requested);
                    std::array<std::uint8_t, kFrameSize> reply{};
                    const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
                    portENTER_CRITICAL(&g_runtime.session_lock);
                    const bool completed = g_runtime.session.CompleteAction(op.generation, op.request.sequence, true, completed_at, reply);
                    portEXIT_CRITICAL(&g_runtime.session_lock);
                    if (completed) Notify(reply);
                } else {
                    std::array<std::uint8_t, kFrameSize> reply{};
                    portENTER_CRITICAL(&g_runtime.session_lock);
                    const bool completed = g_runtime.session.FailAction(op.generation, op.request.sequence, Result::InternalError, reply);
                    portEXIT_CRITICAL(&g_runtime.session_lock);
                    if (completed) Notify(reply);
                }
                continue;
            }
            if (opcode == Opcode::Arm) {
                if (!g_runtime.startup_configured) continue;
                if (g_runtime.motion.valid_mask() == 0) {
                    if (!valid_work(op)) continue;
                    g_runtime.motion.Initialize(op.target.channels);
                    g_runtime.motion.Halt(op.generation);
                    motion_generation = op.generation;
                    if (!write_output()) { fail_output(); continue; }
                    servo_enabled = true;
                }
            } else if (opcode == Opcode::Halt || opcode == Opcode::Release) {
                g_runtime.motion.Halt(op.generation);
                motion_generation = op.generation;
            } else {
                continue;
            }
            std::array<std::uint8_t, kFrameSize> reply{};
            const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            portENTER_CRITICAL(&g_runtime.session_lock);
            const bool completed = g_runtime.session.CompleteAction(op.generation, op.request.sequence, true, completed_at, reply);
            portEXIT_CRITICAL(&g_runtime.session_lock);
            if (completed) {
                if (opcode == Opcode::Halt) RecordStop(StopReason::Halt);
                if (opcode == Opcode::Release) RecordStop(StopReason::Release);
                g_runtime.motion.SetLastSequence(op.request.sequence);
                publish_motion(op.generation, op.request.sequence);
                Notify(reply);
            }
            // RELEASE's bounded cache window is managed by TickLease, so
            // duplicate RELEASE can recover a lost ACK without blocking ticks.
        }
        if (has_target && servo_enabled && valid_work(latest_target)) {
            if (motion_generation != latest_target.generation) {
                g_runtime.motion.Halt(latest_target.generation);
                motion_generation = latest_target.generation;
            }
            g_runtime.motion.SetTarget(latest_target.target, now, latest_target.generation);
            if (latest_target.target.transition_ms == 0 && valid_work(latest_target)) {
                if (!write_output()) { fail_output(); continue; }
            }
            std::array<std::uint8_t, kFrameSize> ignored_reply{};
            const auto completed_at = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            portENTER_CRITICAL(&g_runtime.session_lock);
            const bool completed = g_runtime.session.CompleteAction(latest_target.generation,
                latest_target.request.sequence, true, completed_at, ignored_reply);
            portEXIT_CRITICAL(&g_runtime.session_lock);
            if (completed) {
                g_runtime.motion.SetLastSequence(latest_target.request.sequence);
                publish_motion(latest_target.generation, latest_target.request.sequence);
            }
        }
        portENTER_CRITICAL(&g_runtime.session_lock);
        generation = g_runtime.session.generation();
        portEXIT_CRITICAL(&g_runtime.session_lock);
        if (generation != motion_generation) {
            g_runtime.motion.Halt(generation);
            motion_generation = generation;
        } else if (servo_enabled && g_runtime.motion.Tick(now, generation)) {
            if (write_output()) {
                publish_motion(generation, state_now().last_applied_sequence);
            } else {
                fail_output();
            }
        }
        if (connection != kNoConnection && now - last_state_event >= 500) {
            last_state_event = now;
            NotifySnapshotEvent();
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MotionEngine::kTickMs));
    }
}

} // namespace satori::ble::internal
#endif

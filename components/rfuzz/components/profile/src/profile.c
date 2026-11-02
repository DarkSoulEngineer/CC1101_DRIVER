#include "profile.h"
#include "tx.h"
#include "rx.h"
#include "hw_init.h"
#include "esp_log.h"

static const char *TAG = "RFUZZ_PROFILE";

void rfuzz_profile_init(rfuzz_profile_t *s)
{
    *s = (rfuzz_profile_t){
        .requested_mod = CC1101_PROFILE_OOK, .applied_mod = CC1101_PROFILE_OOK,
        .requested_mode = CC1101_RX_PACKET, .applied_mode = CC1101_RX_PACKET,
        .pending = true, .apply_both = true
    };
}

void rfuzz_profile_mod(rfuzz_profile_t *s, cc1101_modulation_t modulation)
{
    s->requested_mod = modulation;
    s->pending = true;
    s->apply_both = true;
}

bool rfuzz_profile_mode(rfuzz_profile_t *s, cc1101_rx_format_t mode)
{
    if (!s->valid && !s->pending) return false;
    s->requested_mode = mode;
    s->pending = true;
    return true;
}

static void cleanup_locked(void)
{
    rfuzz_rx_capture_stop();
    rfuzz_tx_idle_locked();
    rfuzz_rx_idle_locked();
}

esp_err_t rfuzz_profile_process(rfuzz_profile_t *s)
{
    if (s->cleanup_pending) {
        if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        cleanup_locked();
        s->cleanup_pending = false;
        xSemaphoreGive(cc1101_bus_mutex);
        return ESP_OK;
    }
    if (!s->pending) return ESP_OK;
    s->valid = false;
    esp_err_t err = rfuzz_rx_capture_stop();
    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        s->pending = false;
        s->apply_both = false;
        s->cleanup_pending = true;
        ESP_LOGE(TAG, "Profile bus timeout; invalid. Recover with 'mod ...'.");
        return ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK && s->apply_both)
        err = rfuzz_tx_apply_profile_locked(s->requested_mod);
    if (err == ESP_OK)
        err = rfuzz_rx_apply_profile_locked(s->requested_mod, s->requested_mode);
    if (err == ESP_OK) err = rfuzz_rx_start_locked();

    cc1101_profile_status_t a, b;
    if (err == ESP_OK) err = rfuzz_tx_profile_readback_locked(&a);
    if (err == ESP_OK) err = rfuzz_rx_profile_readback_locked(&b);
    if (err == ESP_OK) {
        s->applied_mod = s->requested_mod;
        s->applied_mode = s->requested_mode;
        s->valid = true;
    } else {
        cleanup_locked();
    }
    xSemaphoreGive(cc1101_bus_mutex);
    s->pending = false;
    s->apply_both = false;
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Profile transition OK: valid=%s", s->valid ? "yes" : "no");
    else {
        ESP_LOGE(TAG, "Profile transition FAILED: %s (valid=%s)", esp_err_to_name(err), s->valid ? "yes" : "no");
        ESP_LOGE(TAG, "Capture stopped; A/B set to IDLE (best effort). Recover with 'mod ...'.");
    }
    return err;
}

void rfuzz_profile_check(rfuzz_profile_t *s)
{
    if (s->valid && !rfuzz_rx_is_raw() && !rfuzz_rx_is_packet()) {
        s->valid = false;
        if (!s->apply_both) s->pending = false;
        rfuzz_rx_capture_stop();
        s->cleanup_pending = true;
        ESP_LOGE(TAG, "Capture invalid; recover with 'mod ...'.");
    }
    if (s->cleanup_pending) rfuzz_profile_process(s);
}

static const char *mod_name(cc1101_modulation_t mod)
{
    switch (mod) {
    case CC1101_PROFILE_OOK: return "OOK";
    case CC1101_PROFILE_ASK: return "ASK";
    case CC1101_PROFILE_2FSK: return "2FSK";
    case CC1101_PROFILE_GFSK: return "GFSK";
    default: return "unknown";
    }
}

static void snapshot(const char *name, esp_err_t err, const cc1101_profile_status_t *p)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s actual readback unavailable: %s", name, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "  %s PKTCTRL0=%02X  MDMCFG2=%02X  DEVIATN=%02X", name, p->pktctrl0, p->mdmcfg2, p->deviatn);
    ESP_LOGI(TAG, "  %s AGC=%02X/%02X/%02X  FOC=%02X  FREND0=%02X  IOCFG0=%02X  MARCSTATE=%02X",
             name, p->agcctrl2, p->agcctrl1, p->agcctrl0, p->foccfg, p->frend0, p->iocfg0, p->marcstate);
    ESP_LOGI(TAG, "  %s PA=%02X %02X %02X %02X %02X %02X %02X %02X",
             name, p->patable[0], p->patable[1], p->patable[2], p->patable[3],
             p->patable[4], p->patable[5], p->patable[6], p->patable[7]);
}

void rfuzz_profile_status(const rfuzz_profile_t *s)
{
    ESP_LOGI(TAG, "=== Profile state ===");
    ESP_LOGI(TAG, "  requested: mod=%s  mode=%s",
             mod_name(s->requested_mod), s->requested_mode == CC1101_RX_PACKET ? "packet" : "raw");
    ESP_LOGI(TAG, "  applied:   mod=%s  mode=%s",
             mod_name(s->applied_mod), s->applied_mode == CC1101_RX_PACKET ? "packet" : "raw");
    ESP_LOGI(TAG, "  valid=%s  pending=%s  cleanup_pending=%s",
             s->valid ? "yes" : "no", s->pending ? "yes" : "no", s->cleanup_pending ? "yes" : "no");
    ESP_LOGI(TAG, "");
    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        cc1101_profile_status_t a, b;
        esp_err_t ea = rfuzz_tx_profile_readback_locked(&a);
        esp_err_t eb = rfuzz_rx_profile_readback_locked(&b);
        xSemaphoreGive(cc1101_bus_mutex);
        ESP_LOGI(TAG, "=== Radio A (TX) actual readback ===");
        snapshot("A", ea, &a);
        ESP_LOGI(TAG, "=== Radio B (RX) actual readback ===");
        snapshot("B", eb, &b);
    } else {
        ESP_LOGW(TAG, "A/B actual readbacks unavailable: bus timeout");
    }
    ESP_LOGI(TAG, "");
    rfuzz_rx_status();
}

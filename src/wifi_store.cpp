#if defined(TRANSPORT_MESHTASTIC)

#include "wifi_store.h"
#include <Preferences.h>
#include <string.h>

struct WifiNet {
    char     ssid[WIFI_SSID_LEN];
    char     pass[WIFI_PASS_LEN];
    uint32_t rank;   // higher = more recently used
};

struct WifiTable {
    uint8_t  count;
    uint32_t next_rank;
    WifiNet  net[WIFI_STORE_MAX];
};

static WifiTable s_tbl;
static Preferences s_prefs;

static void persist() {
    s_prefs.begin("gsf_wifi", false);
    s_prefs.putBytes("tbl", &s_tbl, sizeof(s_tbl));
    s_prefs.end();
}

void wifi_store_begin() {
    memset(&s_tbl, 0, sizeof(s_tbl));
    s_tbl.next_rank = 1;
    s_prefs.begin("gsf_wifi", true);           // read-only
    size_t n = s_prefs.getBytesLength("tbl");
    if (n == sizeof(s_tbl)) s_prefs.getBytes("tbl", &s_tbl, sizeof(s_tbl));
    s_prefs.end();
    if (s_tbl.count > WIFI_STORE_MAX) s_tbl.count = 0;   // corrupt guard
    if (s_tbl.next_rank == 0) s_tbl.next_rank = 1;
}

int wifi_store_count() { return s_tbl.count; }

static int find_ssid(const char *ssid) {
    for (int i = 0; i < s_tbl.count; i++)
        if (strncmp(s_tbl.net[i].ssid, ssid, WIFI_SSID_LEN) == 0) return i;
    return -1;
}

int wifi_store_by_recency(int *idx_out, int max) {
    int n = s_tbl.count;
    for (int i = 0; i < n; i++) idx_out[i < max ? i : max - 1] = i;
    // simple insertion sort by rank desc (n <= 8)
    for (int i = 1; i < n; i++) {
        int key = idx_out[i], j = i - 1;
        while (j >= 0 && s_tbl.net[idx_out[j]].rank < s_tbl.net[key].rank) {
            idx_out[j + 1] = idx_out[j]; j--;
        }
        idx_out[j + 1] = key;
    }
    return n < max ? n : max;
}

const char *wifi_store_ssid(int idx) {
    return (idx >= 0 && idx < s_tbl.count) ? s_tbl.net[idx].ssid : "";
}
const char *wifi_store_pass(int idx) {
    return (idx >= 0 && idx < s_tbl.count) ? s_tbl.net[idx].pass : "";
}

void wifi_store_remember(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;

    int i = find_ssid(ssid);
    if (i < 0) {
        if (s_tbl.count < WIFI_STORE_MAX) {
            i = s_tbl.count++;
        } else {
            // full: evict the least-recently-used (lowest rank)
            i = 0;
            for (int k = 1; k < s_tbl.count; k++)
                if (s_tbl.net[k].rank < s_tbl.net[i].rank) i = k;
        }
        memset(&s_tbl.net[i], 0, sizeof(WifiNet));
        strncpy(s_tbl.net[i].ssid, ssid, WIFI_SSID_LEN - 1);
    }
    if (pass) {
        strncpy(s_tbl.net[i].pass, pass, WIFI_PASS_LEN - 1);
        s_tbl.net[i].pass[WIFI_PASS_LEN - 1] = '\0';
    }
    s_tbl.net[i].rank = s_tbl.next_rank++;
    persist();
}

void wifi_store_touch(const char *ssid) {
    int i = find_ssid(ssid);
    if (i < 0) return;
    s_tbl.net[i].rank = s_tbl.next_rank++;
    persist();
}

bool wifi_store_delete_idx(int idx) {
    if (idx < 0 || idx >= s_tbl.count) return false;
    for (int k = idx; k < s_tbl.count - 1; k++) s_tbl.net[k] = s_tbl.net[k + 1];
    s_tbl.count--;
    memset(&s_tbl.net[s_tbl.count], 0, sizeof(WifiNet));
    persist();
    return true;
}

bool wifi_store_delete(const char *ssid) {
    return wifi_store_delete_idx(find_ssid(ssid));
}

#endif // TRANSPORT_MESHTASTIC

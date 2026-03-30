// main.c - Mech Badge firmware
// BLE proximity + ESP-NOW match + deterministic combat (from mech_idle.cpp)

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "mbedtls/md.h"

static const char *TAG = "BADGE";

// ---- per-badge config (edit before flashing) ----
// 0x00 = auto-derive from MAC

#define BADGE_FACTION_ID    0x01    // Davion=01 Kurita=02 Steiner=03
#define BADGE_MECH_CLASS    0x00    // auto=00 Light=01 Med=02 Heavy=03 Assault=04
#define BADGE_RANK          0x05
#define BADGE_RECORD        0x31    // hi=wins lo=losses
#define BADGE_PRIMARY_SAO   0x02
#define BADGE_SECONDARY_SAO 0xFF
#define BADGE_FLAGS         0x00
#define MECH_UUID_LOBBY     0xAB01

static uint32_t g_player_id  = 0;
static uint8_t  g_badge_id   = 0;
static uint8_t  g_mech_class = 0;

// ---- event keys (REPLACE before production) ----

static const uint8_t BADGE_HMAC_KEY[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t BADGE_PMK[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ---- tuning ----

#define MAX_TRACKED       8
#define RSSI_ALPHA        3
#define RSSI_VERY_CLOSE   -60
#define RSSI_CLOSE        -70
#define RSSI_MEDIUM       -80
#define TREND_THRESHOLD   3
#define STATE_UPDATE_MS   200
#define MAX_TURNS         30
#define TURN_TIMEOUT_MS   15000
#define INVITE_COOLDOWN_MS 5000
#define MAX_RX_PER_SEC    20

// ---- splitmix64 ----

typedef struct { uint64_t state; } Rng;

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void    rng_seed(Rng *r, uint64_t s) { r->state = s; }
static uint32_t rng_u32(Rng *r) { uint64_t x = r->state; uint64_t z = splitmix64(&x); r->state = x; return (uint32_t)(z & 0xFFFFFFFFu); }
static int      rng_int(Rng *r, int lo, int hi) { if (lo > hi) { int t = lo; lo = hi; hi = t; } return lo + (int)(rng_u32(r) % (uint32_t)(hi - lo + 1)); }
static int      roll_2d6(Rng *r) { return rng_int(r, 1, 6) + rng_int(r, 1, 6); }

static uint64_t make_seed(uint32_t a, uint32_t b, uint32_t salt)
{
    uint64_t lo = (a < b) ? a : b, hi = (a < b) ? b : a;
    uint64_t x = (hi << 32) ^ lo ^ ((uint64_t)salt << 1);
    return splitmix64(&x);
}

// ---- game types ----

typedef enum { RANGE_MELEE, RANGE_SHORT, RANGE_MED, RANGE_LONG } RangeBand;
typedef enum { ACT_CLOSE, ACT_HOLD, ACT_OPEN } RangeAction;
typedef enum { LOC_HEAD, LOC_CT, LOC_RT, LOC_LT, LOC_RA, LOC_LA, LOC_RL, LOC_LL, LOC_COUNT } Loc;

typedef struct { char name[16]; int damage, heat; RangeBand min_range, max_range; } Weapon;
typedef struct { char chassis[16]; int speed, heat_sinks, heat, shutdown, move_penalty; int armor[LOC_COUNT], internal[LOC_COUNT]; Weapon weapons[3]; } Mech;
typedef struct { int gunnery, piloting; } Pilot;
typedef struct { RangeAction action; int fire[3]; } PlayerChoice;

static uint8_t pack_choice(const PlayerChoice *c)
{
    return (uint8_t)((c->action & 0x03) | ((c->fire[0] & 1) << 2) | ((c->fire[1] & 1) << 3) | ((c->fire[2] & 1) << 4));
}

static PlayerChoice unpack_choice(uint8_t b)
{
    return (PlayerChoice){ .action = (RangeAction)(b & 0x03), .fire = { (b>>2)&1, (b>>3)&1, (b>>4)&1 } };
}

// ---- mech definitions ----

static Weapon w_med_laser(void) { return (Weapon){"Med Laser", 5, 3, RANGE_SHORT, RANGE_MED}; }
static Weapon w_ppc(void)       { return (Weapon){"PPC",      10,10, RANGE_MED,   RANGE_LONG}; }
static Weapon w_ac5(void)       { return (Weapon){"AC/5",      5, 1, RANGE_SHORT, RANGE_LONG}; }
static Weapon w_lrm10(void)     { return (Weapon){"LRM-10",   10, 4, RANGE_MED,   RANGE_LONG}; }
static Weapon w_srm4(void)      { return (Weapon){"SRM-4",     8, 3, RANGE_MELEE, RANGE_SHORT}; }

static Mech make_loki(void)
{
    Mech m = {0};
    strcpy(m.chassis, "SUMMONER");
    m.speed = 3; m.heat_sinks = 6;
    int a[] = {3,18,12,12,8,8,10,10}, s[] = {3,12,8,8,6,6,7,7};
    memcpy(m.armor, a, sizeof(a)); memcpy(m.internal, s, sizeof(s));
    m.weapons[0] = w_ppc(); m.weapons[1] = w_med_laser(); m.weapons[2] = w_srm4();
    return m;
}

static Mech make_bushwacker(void)
{
    Mech m = {0};
    strcpy(m.chassis, "BUSHWACKER");
    m.speed = 2; m.heat_sinks = 5;
    int a[] = {3,20,13,13,9,9,11,11}, s[] = {3,13,9,9,6,6,8,8};
    memcpy(m.armor, a, sizeof(a)); memcpy(m.internal, s, sizeof(s));
    m.weapons[0] = w_ac5(); m.weapons[1] = w_lrm10(); m.weapons[2] = w_med_laser();
    return m;
}

static Mech mech_for_class(uint8_t cls) { return (cls <= 0x02) ? make_bushwacker() : make_loki(); }

// ---- combat ----

static int  mech_alive(const Mech *m) { return m->internal[LOC_CT] > 0 && m->internal[LOC_HEAD] > 0; }
static int  eff_speed(const Mech *m)  { int s = m->speed - m->move_penalty; return s > 0 ? s : 0; }

static Loc hit_location(int roll)
{
    switch (roll) {
    case 2: return LOC_CT; case 3: case 4: return LOC_RA; case 5: return LOC_RL;
    case 6: return LOC_RT; case 7: return LOC_CT; case 8: return LOC_LT;
    case 9: return LOC_LL; case 10: case 11: return LOC_LA; case 12: return LOC_HEAD;
    default: return LOC_CT;
    }
}

static void apply_damage(Mech *m, Loc loc, int dmg, Rng *rng)
{
    int spill = dmg - m->armor[loc];
    if (spill <= 0) { m->armor[loc] -= dmg; return; }
    m->armor[loc] = 0;
    m->internal[loc] -= spill;
    if (m->internal[loc] < 0) m->internal[loc] = 0;
    if (roll_2d6(rng) >= 8) ESP_LOGI(TAG, "  CRIT on %d!", loc);
}

static void sink_heat(Mech *m) { m->heat -= m->heat_sinks; if (m->heat < 0) m->heat = 0; }

static void heat_phase(Mech *m, Pilot *p, Rng *rng)
{
    if (m->shutdown) {
        if (roll_2d6(rng) + (6 - p->piloting) >= 8) { m->shutdown = 0; ESP_LOGI(TAG, "  %s restarted", m->chassis); }
    }
    m->move_penalty = (m->heat >= 15) ? 3 : (m->heat >= 10) ? 2 : (m->heat >= 5) ? 1 : 0;
    if (m->heat >= 19) m->shutdown = 1;
    else if (m->heat >= 14 && roll_2d6(rng) >= 8) m->shutdown = 1;
    if (m->shutdown) ESP_LOGW(TAG, "  %s SHUTDOWN (heat=%d)", m->chassis, m->heat);
}

static int heat_mod(int h) { return (h >= 13) ? 4 : (h >= 8) ? 2 : (h >= 5) ? 1 : 0; }
static int range_mod(RangeBand r) { return (r == RANGE_LONG) ? 4 : (r == RANGE_MED) ? 2 : 0; }

static void resolve_attacks(Mech *atk, Pilot *p, Mech *def, PlayerChoice *c, RangeBand range, int move_mod, Rng *rng)
{
    if (atk->shutdown || !mech_alive(atk)) return;
    for (int w = 0; w < 3; w++) {
        if (!c->fire[w]) continue;
        Weapon *wp = &atk->weapons[w];
        atk->heat += wp->heat;
        if (range < wp->min_range || range > wp->max_range) { ESP_LOGI(TAG, "  %s out of range", wp->name); continue; }
        int tn = 2 + p->gunnery + range_mod(range) + heat_mod(atk->heat) + move_mod;
        int r = roll_2d6(rng);
        if (r < tn) { ESP_LOGI(TAG, "  %s missed (%d vs %d)", wp->name, r, tn); continue; }
        Loc loc = hit_location(roll_2d6(rng));
        ESP_LOGI(TAG, "  %s hit loc %d for %d", wp->name, loc, wp->damage);
        apply_damage(def, loc, wp->damage, rng);
        if (!mech_alive(def)) { ESP_LOGW(TAG, "  %s destroyed!", def->chassis); return; }
    }
}

static void run_turn(int turn, RangeBand *range, Mech *a, Pilot *pa, PlayerChoice *ca, Mech *b, Pilot *pb, PlayerChoice *cb, Rng *rng)
{
    ESP_LOGI(TAG, "-- turn %d range=%d heat:%s=%d %s=%d --", turn, *range, a->chassis, a->heat, b->chassis, b->heat);
    int init_a = roll_2d6(rng) + eff_speed(a), init_b = roll_2d6(rng) + eff_speed(b);
    int roll_a = roll_2d6(rng) + eff_speed(a) + (6 - pa->piloting);
    int roll_b = roll_2d6(rng) + eff_speed(b) + (6 - pb->piloting);
    RangeAction want = (roll_a >= roll_b) ? ca->action : cb->action;
    if (want == ACT_CLOSE && *range > RANGE_MELEE) (*range)--;
    if (want == ACT_OPEN  && *range < RANGE_LONG)  (*range)++;
    int ma = (eff_speed(a) > 2) ? 1 : 0, mb = (eff_speed(b) > 2) ? 1 : 0;
    if (init_a >= init_b) { resolve_attacks(a, pa, b, ca, *range, mb, rng); resolve_attacks(b, pb, a, cb, *range, ma, rng); }
    else                  { resolve_attacks(b, pb, a, cb, *range, ma, rng); resolve_attacks(a, pa, b, ca, *range, mb, rng); }
    sink_heat(a); sink_heat(b); heat_phase(a, pa, rng); heat_phase(b, pb, rng);
}

// ---- packets ----
// match_id binds all packets to one session, prevents cross-match injection
// target_id in invite prevents hijacking by bystanders

#define PKT_MATCH_INVITE  0x03
#define PKT_MATCH_ACCEPT  0x04
#define PKT_MATCH_START   0x05
#define PKT_MATCH_END     0x06
#define PKT_TURN_CHOICE   0x07

typedef struct __attribute__((packed)) {
    uint8_t protoVersion; uint32_t playerID; uint8_t factionID, mechClass, primarySAO, secondarySAO, playerRank, matchRecord, badgeFlags, txPower;
} BadgeAdvPayload;

typedef struct __attribute__((packed)) {
    uint8_t  pkt_type, host_id, target_id;
    uint32_t host_player_id, salt, match_id;
    uint8_t  host_mech_class;
    uint16_t seq;
    uint8_t  hmac[8];
} MatchInvitePacket;

typedef struct __attribute__((packed)) {
    uint8_t  pkt_type, accept_id;
    uint32_t accept_player_id, match_id;
    uint8_t  accept_mech_class;
    uint16_t seq;
    uint8_t  hmac[8];
} MatchAcceptPacket;

typedef struct __attribute__((packed)) {
    uint8_t  pkt_type, host_id;
    uint32_t match_id;
    uint16_t seq;
    uint8_t  hmac[8];
} MatchStartPacket;

typedef struct __attribute__((packed)) {
    uint8_t  pkt_type, badge_id, turn, choice;
    uint32_t match_id;
    uint16_t seq;
    uint8_t  hmac[8];
} TurnChoicePacket;

typedef struct __attribute__((packed)) {
    uint8_t  pkt_type, badge_id, winner_id, turns_played;
    uint32_t match_id;
    uint16_t seq;
    uint8_t  hmac[8];
} MatchEndPacket;

// ---- state ----

typedef struct { uint8_t addr[6], badge_id, mech_class; uint32_t player_id, last_seen_ms; int16_t rssi_smooth, rssi_prev; } TrackedDevice;
typedef enum { STATE_IDLE, STATE_INVITE_SENT, STATE_LOBBY, STATE_COMBAT, STATE_RESULT } BadgeState;

// ---- globals ----

static BadgeAdvPayload k_payload = {
    .protoVersion = 0x01, .playerID = 0, .factionID = BADGE_FACTION_ID, .mechClass = 0,
    .primarySAO = BADGE_PRIMARY_SAO, .secondarySAO = BADGE_SECONDARY_SAO,
    .playerRank = BADGE_RANK, .matchRecord = BADGE_RECORD, .badgeFlags = BADGE_FLAGS, .txPower = 0,
};

static const char   *PROX_STR[] = {"VERY CLOSE", "CLOSE", "MEDIUM", "FAR"};
static const uint8_t BCAST[6]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static TrackedDevice g_tracked[MAX_TRACKED];
static uint8_t       g_my_ble_mac[6];
static uint16_t      g_seq;
static BadgeState    g_state = STATE_IDLE;
static esp_timer_handle_t g_tick_timer;

static uint32_t     g_match_id; // random per match, binds all packets
static uint32_t     g_salt;
static uint8_t      g_opp_id, g_opp_mech_class, g_opp_wifi_mac[6];
static uint32_t     g_opp_player_id;
static bool         g_is_host;
static Rng          g_rng;
static Mech         g_my_mech, g_opp_mech;
static Pilot        g_my_pilot, g_opp_pilot;
static RangeBand    g_range;
static int          g_turn;
static PlayerChoice g_my_choice, g_opp_choice;
static bool         g_my_choice_sent, g_opp_choice_recv;
static uint32_t     g_turn_start_ms, g_last_choice_send_ms;
static uint32_t     g_last_invite_ms; // invite cooldown
static uint32_t     g_rx_count;       // per-second rx counter
static uint32_t     g_rx_window_ms;

// ---- helpers ----

static uint8_t rssi_to_proximity(int8_t r) { return (r >= RSSI_VERY_CLOSE) ? 0 : (r >= RSSI_CLOSE) ? 1 : (r >= RSSI_MEDIUM) ? 2 : 3; }

static bool adv_has_uuid16(const uint8_t *d, uint8_t len, uint16_t target)
{
    for (uint8_t i = 0; i < len; ) {
        uint8_t flen = d[i];
        if (!flen || (i + flen) >= len) break;
        if (d[i+1] == 0x02 || d[i+1] == 0x03)
            for (uint8_t j = 2; j+1 <= flen; j += 2)
                if ((uint16_t)(d[i+j] | (d[i+j+1] << 8)) == target) return true;
        i += flen + 1;
    }
    return false;
}

static bool parse_badge_adv(const uint8_t *data, uint8_t len, BadgeAdvPayload *out)
{
    for (uint8_t i = 0; i < len; ) {
        uint8_t flen = data[i];
        if (!flen || i + 1 + flen > len) break;
        if (data[i+1] == 0xFF && flen >= 3 + (uint8_t)sizeof(BadgeAdvPayload)) {
            memcpy(out, &data[i+4], sizeof(BadgeAdvPayload));
            return true;
        }
        i += 1 + flen;
    }
    return false;
}

static int8_t smooth_rssi(TrackedDevice *dev, int8_t rssi)
{
    dev->rssi_prev = dev->rssi_smooth;
    if (!dev->rssi_smooth) dev->rssi_smooth = (int16_t)rssi * 8;
    else dev->rssi_smooth += RSSI_ALPHA * (rssi - dev->rssi_smooth / 8);
    return (int8_t)(dev->rssi_smooth / 8);
}

static const char *trend_str(const TrackedDevice *dev)
{
    if (!dev->rssi_prev) return "  ";
    int8_t c = (int8_t)(dev->rssi_smooth / 8), p = (int8_t)(dev->rssi_prev / 8);
    return (c - p > TREND_THRESHOLD) ? ">>" : (c - p < -TREND_THRESHOLD) ? "<<" : "--";
}

static TrackedDevice *get_device(const uint8_t *addr)
{
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (!memcmp(g_tracked[i].addr, addr, 6)) return &g_tracked[i];
        if (free_slot < 0 && !g_tracked[i].last_seen_ms) free_slot = i;
        if (g_tracked[i].last_seen_ms < g_tracked[oldest].last_seen_ms) oldest = i;
    }
    int s = (free_slot >= 0) ? free_slot : oldest;
    memset(&g_tracked[s], 0, sizeof(TrackedDevice));
    memcpy(g_tracked[s].addr, addr, 6);
    return &g_tracked[s];
}

// ---- hmac (truncated sha256, first 8 bytes) ----

static void compute_hmac(const void *data, size_t len, uint8_t out[8])
{
    uint8_t full[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    BADGE_HMAC_KEY, sizeof(BADGE_HMAC_KEY),
                    (const uint8_t *)data, len, full);
    memcpy(out, full, 8);
}

static bool verify_hmac(const void *data, size_t len, const uint8_t expected[8])
{
    uint8_t got[8]; compute_hmac(data, len, got);
    // constant-time compare to prevent timing attacks
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= got[i] ^ expected[i];
    return diff == 0;
}

static void register_espnow_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t p = { .ifidx = ESP_IF_WIFI_STA };
        memcpy(p.peer_addr, mac, 6);
        esp_now_add_peer(&p);
    }
}

static void send_to_opp(const void *data, size_t len)
{
    if (g_opp_wifi_mac[0]) { register_espnow_peer(g_opp_wifi_mac); esp_now_send(g_opp_wifi_mac, data, len); }
    else esp_now_send(BCAST, data, len);
}

// drop if sender isn't our current opponent (during match)
static bool is_from_opponent(const uint8_t *src_addr)
{
    return memcmp(src_addr, g_opp_wifi_mac, 6) == 0;
}

// ---- match flow ----

void start_match_with(uint8_t target_id)
{
    if (g_state != STATE_IDLE) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now - g_last_invite_ms < INVITE_COOLDOWN_MS) return;

    TrackedDevice *t = NULL;
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_tracked[i].badge_id == target_id && g_tracked[i].last_seen_ms) { t = &g_tracked[i]; break; }
    if (!t) return;

    g_is_host = true; g_opp_id = target_id;
    g_salt     = esp_random();
    g_match_id = esp_random();
    g_last_invite_ms = now;

    MatchInvitePacket p = {
        .pkt_type = PKT_MATCH_INVITE, .host_id = g_badge_id, .target_id = target_id,
        .host_player_id = g_player_id, .salt = g_salt, .match_id = g_match_id,
        .host_mech_class = g_mech_class, .seq = g_seq++
    };
    compute_hmac(&p, offsetof(MatchInvitePacket, hmac), p.hmac);
    esp_now_send(BCAST, (const uint8_t *)&p, sizeof(p));
    g_state = STATE_INVITE_SENT;
    ESP_LOGI(TAG, "invited %02X match=%08lX", target_id, (unsigned long)g_match_id);
}

static void begin_combat(void)
{
    rng_seed(&g_rng, make_seed(g_player_id, g_opp_player_id, g_salt));
    g_my_mech = mech_for_class(g_mech_class);
    g_opp_mech = mech_for_class(g_opp_mech_class);
    g_my_pilot  = (Pilot){2, 5};
    g_opp_pilot = (Pilot){3, 5};
    g_range = RANGE_LONG; g_turn = 0;
    g_my_choice_sent = g_opp_choice_recv = false;
    g_state = STATE_COMBAT;
    g_turn_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    ESP_LOGI(TAG, "COMBAT: %s vs %s match=%08lX", g_my_mech.chassis, g_opp_mech.chassis, (unsigned long)g_match_id);
    g_my_choice = (PlayerChoice){ACT_CLOSE, {1,1,1}};
}

static void send_my_choice(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (g_my_choice_sent && (now - g_last_choice_send_ms < 500)) return;
    TurnChoicePacket p = { .pkt_type = PKT_TURN_CHOICE, .badge_id = g_badge_id, .turn = (uint8_t)g_turn, .choice = pack_choice(&g_my_choice), .match_id = g_match_id, .seq = g_seq++ };
    compute_hmac(&p, offsetof(TurnChoicePacket, hmac), p.hmac);
    send_to_opp(&p, sizeof(p));
    g_my_choice_sent = true; g_last_choice_send_ms = now;
}

static void advance_turn(void)
{
    bool first = (g_badge_id <= g_opp_id);
    run_turn(g_turn, &g_range,
             first ? &g_my_mech : &g_opp_mech, first ? &g_my_pilot : &g_opp_pilot, first ? &g_my_choice : &g_opp_choice,
             first ? &g_opp_mech : &g_my_mech, first ? &g_opp_pilot : &g_my_pilot, first ? &g_opp_choice : &g_my_choice,
             &g_rng);

    g_turn++;
    g_my_choice_sent = g_opp_choice_recv = false;
    g_turn_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    bool dead_me = !mech_alive(&g_my_mech), dead_opp = !mech_alive(&g_opp_mech);
    if (dead_me || dead_opp || g_turn >= MAX_TURNS) {
        uint8_t winner = 0;
        if (!dead_me && dead_opp) winner = g_badge_id;
        if (dead_me && !dead_opp) winner = g_opp_id;
        ESP_LOGW(TAG, "=== %s ===", winner == g_badge_id ? "VICTORY" : winner == g_opp_id ? "DEFEAT" : "DRAW");

        MatchEndPacket ep = { .pkt_type = PKT_MATCH_END, .badge_id = g_badge_id, .winner_id = winner, .turns_played = (uint8_t)g_turn, .match_id = g_match_id, .seq = g_seq++ };
        compute_hmac(&ep, offsetof(MatchEndPacket, hmac), ep.hmac);
        send_to_opp(&ep, sizeof(ep));
        g_state = STATE_RESULT;
        ESP_LOGI(TAG, "match %08lX over, %d turns", (unsigned long)g_match_id, g_turn);
        return;
    }

    g_my_choice = (g_range > RANGE_SHORT)
        ? (PlayerChoice){ACT_CLOSE, {1,1,1}}
        : (PlayerChoice){ACT_HOLD,  {1,1,1}};
}

// ---- rx handlers ----

static void handle_invite(const esp_now_recv_info_t *info, const MatchInvitePacket *p)
{
    if (!verify_hmac(p, offsetof(MatchInvitePacket, hmac), p->hmac)) return;
    if (g_state != STATE_IDLE) return;
    if (p->target_id != g_badge_id) return; // not for us

    g_is_host = false; g_opp_id = p->host_id; g_opp_player_id = p->host_player_id;
    g_opp_mech_class = p->host_mech_class; g_salt = p->salt; g_match_id = p->match_id;
    memcpy(g_opp_wifi_mac, info->src_addr, 6);
    ESP_LOGI(TAG, "invite from %02X match=%08lX", p->host_id, (unsigned long)g_match_id);

    MatchAcceptPacket ap = { .pkt_type = PKT_MATCH_ACCEPT, .accept_id = g_badge_id, .accept_player_id = g_player_id, .match_id = g_match_id, .accept_mech_class = g_mech_class, .seq = g_seq++ };
    compute_hmac(&ap, offsetof(MatchAcceptPacket, hmac), ap.hmac);
    send_to_opp(&ap, sizeof(ap));
    g_state = STATE_LOBBY;
}

static void handle_accept(const esp_now_recv_info_t *info, const MatchAcceptPacket *p)
{
    if (!verify_hmac(p, offsetof(MatchAcceptPacket, hmac), p->hmac)) return;
    if (g_state != STATE_INVITE_SENT) return;
    if (p->match_id != g_match_id) return;

    g_opp_id = p->accept_id; g_opp_player_id = p->accept_player_id; g_opp_mech_class = p->accept_mech_class;
    memcpy(g_opp_wifi_mac, info->src_addr, 6);
    ESP_LOGI(TAG, "%02X accepted match=%08lX", p->accept_id, (unsigned long)g_match_id);

    MatchStartPacket sp = { .pkt_type = PKT_MATCH_START, .host_id = g_badge_id, .match_id = g_match_id, .seq = g_seq++ };
    compute_hmac(&sp, offsetof(MatchStartPacket, hmac), sp.hmac);
    send_to_opp(&sp, sizeof(sp));
    begin_combat();
}

static void handle_start(const esp_now_recv_info_t *info, const MatchStartPacket *p)
{
    if (!verify_hmac(p, offsetof(MatchStartPacket, hmac), p->hmac)) return;
    if (g_state != STATE_LOBBY) return;
    if (p->match_id != g_match_id) return;
    if (!is_from_opponent(info->src_addr)) return;
    ESP_LOGI(TAG, "match %08lX started", (unsigned long)g_match_id);
    begin_combat();
}

static void handle_turn_choice(const esp_now_recv_info_t *info, const TurnChoicePacket *p)
{
    if (!verify_hmac(p, offsetof(TurnChoicePacket, hmac), p->hmac)) return;
    if (g_state != STATE_COMBAT) return;
    if (p->match_id != g_match_id) return;
    if (p->turn != (uint8_t)g_turn || g_opp_choice_recv) return;
    if (!is_from_opponent(info->src_addr)) return;
    g_opp_choice = unpack_choice(p->choice);
    g_opp_choice_recv = true;
    ESP_LOGI(TAG, "got turn %d from %02X", p->turn, p->badge_id);
}

static void handle_match_end(const esp_now_recv_info_t *info, const MatchEndPacket *p)
{
    if (!verify_hmac(p, offsetof(MatchEndPacket, hmac), p->hmac)) return;
    if (p->match_id != g_match_id) return;
    if (!is_from_opponent(info->src_addr)) return;
    ESP_LOGI(TAG, "opp says winner=%02X turns=%d", p->winner_id, p->turns_played);
    if (g_state == STATE_COMBAT) g_state = STATE_RESULT;
}

// ---- espnow dispatch ----

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) return;

    // global rate limit: drop if >MAX_RX_PER_SEC packets in the last second
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now - g_rx_window_ms > 1000) { g_rx_count = 0; g_rx_window_ms = now; }
    if (++g_rx_count > MAX_RX_PER_SEC) return;

    switch (data[0]) {
    case PKT_MATCH_INVITE: if (len == sizeof(MatchInvitePacket))  handle_invite(info, (void*)data);     break;
    case PKT_MATCH_ACCEPT: if (len == sizeof(MatchAcceptPacket))  handle_accept(info, (void*)data);     break;
    case PKT_MATCH_START:  if (len == sizeof(MatchStartPacket))   handle_start(info, (void*)data);      break;
    case PKT_TURN_CHOICE:  if (len == sizeof(TurnChoicePacket))   handle_turn_choice(info, (void*)data); break;
    case PKT_MATCH_END:    if (len == sizeof(MatchEndPacket))     handle_match_end(info, (void*)data);  break;
    }
}

// ---- init ----

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_espnow(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk(BADGE_PMK));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    esp_now_peer_info_t peer = { .ifidx = ESP_IF_WIFI_STA };
    memcpy(peer.peer_addr, BCAST, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

// ---- ble ----

static void start_scan(void);

static int scan_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *d = &ev->disc;
        if (!adv_has_uuid16(d->data, d->length_data, MECH_UUID_LOBBY)) return 0;
        TrackedDevice *dev = get_device(d->addr.val);
        dev->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (!dev->badge_id) {
            BadgeAdvPayload adv;
            if (parse_badge_adv(d->data, d->length_data, &adv)) {
                dev->badge_id   = (uint8_t)(adv.playerID & 0xFF);
                dev->mech_class = adv.mechClass;
                dev->player_id  = adv.playerID;
            }
        }
        int8_t rssi = smooth_rssi(dev, d->rssi);
        ESP_LOGI(TAG, MACSTR " id=%02X %ddBm %s %s", MAC2STR(d->addr.val), dev->badge_id, rssi, PROX_STR[rssi_to_proximity(rssi)], trend_str(dev));

        // auto-engage for testing — only the lowest visible ID initiates 
        // !!remove block start
        if (g_state == STATE_IDLE && dev->badge_id && dev->badge_id != g_badge_id && rssi >= RSSI_CLOSE) {
            // check if we're the lowest ID among all tracked badges
            bool i_am_lowest = true;
            for (int k = 0; k < MAX_TRACKED; k++) {
                if (g_tracked[k].last_seen_ms && g_tracked[k].badge_id && g_tracked[k].badge_id < g_badge_id) {
                    i_am_lowest = false;
                    break;
                }
            }
            if (i_am_lowest) {
                static uint32_t last_auto = 0;
                static uint32_t cooldown = 0;
                if (!cooldown) cooldown = 10000 + (esp_random() % 10000); // 10-20s random
                uint32_t now = dev->last_seen_ms;
                if (now - last_auto > cooldown) {
                    last_auto = now;
                    cooldown = 10000 + (esp_random() % 10000); // re-roll each time
                    start_match_with(dev->badge_id);
                }
            }
        }
        // !! remove block end
    }
    if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) start_scan();
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params p = { .itvl = BLE_GAP_SCAN_ITVL_MS(200), .window = BLE_GAP_SCAN_WIN_MS(100), .filter_policy = BLE_HCI_SCAN_FILT_NO_WL, .passive = 1 };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p, scan_cb, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "scan failed %d", rc);
}

static void start_advertising(void)
{
    uint8_t mfr[2 + sizeof(BadgeAdvPayload)] = {0xFF, 0xFF};
    memcpy(&mfr[2], &k_payload, sizeof(BadgeAdvPayload));
    ble_uuid16_t uuid16 = BLE_UUID16_INIT(MECH_UUID_LOBBY);
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids16 = &uuid16; f.num_uuids16 = 1; f.uuids16_is_complete = 1;
    f.mfg_data = mfr; f.mfg_data_len = sizeof(mfr);
    if (ble_gap_adv_set_fields(&f)) { ESP_LOGE(TAG, "adv fields failed"); return; }
    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND; ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min = BLE_GAP_ADV_ITVL_MS(100); ap.itvl_max = BLE_GAP_ADV_ITVL_MS(110);
    if (ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &ap, NULL, NULL)) { ESP_LOGE(TAG, "adv failed"); return; }
    ESP_LOGI(TAG, "adv: id=%02X player=%08lX class=%d", g_badge_id, (unsigned long)g_player_id, g_mech_class);
}

static void on_sync(void) { start_advertising(); start_scan(); }
static void on_reset(int r) { ESP_LOGW(TAG, "ble reset %d", r); }
static void ble_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

// ---- tick ----

static void tick(void *arg)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (g_state == STATE_COMBAT) {
        send_my_choice();
        if (g_my_choice_sent && g_opp_choice_recv) { advance_turn(); return; }
        if (!g_opp_choice_recv && (now - g_turn_start_ms > TURN_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "turn %d timeout", g_turn);
            g_opp_choice = (PlayerChoice){ACT_HOLD, {0,0,0}};
            g_opp_choice_recv = true;
            advance_turn();
            return;
        }
    }

    static uint32_t result_ts = 0;
    if (g_state == STATE_RESULT) {
        if (!result_ts) result_ts = now;
        if (now - result_ts > 5000) { g_state = STATE_IDLE; result_ts = 0; ESP_LOGI(TAG, "idle"); }
    } else result_ts = 0;

    static uint32_t lobby_ts = 0;
    if (g_state == STATE_INVITE_SENT || g_state == STATE_LOBBY) {
        if (!lobby_ts) lobby_ts = now;
        if (now - lobby_ts > 10000) { g_state = STATE_IDLE; lobby_ts = 0; ESP_LOGW(TAG, "lobby timeout"); }
    } else lobby_ts = 0;
}

// ---- main ----

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);

    esp_read_mac(g_my_ble_mac, ESP_MAC_BT);
    memcpy(&g_player_id, &g_my_ble_mac[2], 4);
    g_badge_id = (uint8_t)(g_player_id & 0xFF);
    g_mech_class = BADGE_MECH_CLASS ? BADGE_MECH_CLASS : (g_my_ble_mac[4] % 4) + 1;
    k_payload.playerID = g_player_id;
    k_payload.mechClass = g_mech_class;

    static const char *CLS[] = {"?","Light","Medium","Heavy","Assault"};
    ESP_LOGI(TAG, "id=0x%02X player=0x%08lX %s mac=" MACSTR, g_badge_id, (unsigned long)g_player_id, CLS[g_mech_class], MAC2STR(g_my_ble_mac));

    init_wifi();
    init_espnow();
    const esp_timer_create_args_t ta = { .callback = tick, .name = "tick" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &g_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_tick_timer, STATE_UPDATE_MS * 1000ULL));
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_device_name_set("MechBadge");
    ble_svc_gap_init();
    nimble_port_freertos_init(ble_task);
}

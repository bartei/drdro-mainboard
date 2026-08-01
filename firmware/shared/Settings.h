/**
 * Copyright © 2026 <Stefano Bertelli>
 * GPL-3.0-or-later. See LICENSE.
 *
 * Persistent settings — shared layout for the app and the IAP bootloader. Both
 * sides read it; both may write it (read-modify-write the whole struct so the
 * other side's fields survive). Validity = magic + CRC32.
 *
 * Power-fail safe via ping-pong across TWO sectors (A = sector 1, B = sector 2):
 * each write goes to the *inactive* slot with an incremented `seq`, so a brown-out
 * mid-erase leaves the previous slot intact. settings_load() picks the valid slot
 * with the newest seq (falls back to defaults if neither is valid).
 *
 * Forward-compatible layout (see the contract above settings_t): `crc` is field 0
 * at a frozen offset, `used_size` makes validation length-based, and new fields are
 * append-only. That lets a newer firmware add variables without breaking an older
 * binary's ability to validate/update the image (notably the separately-flashed
 * bootloader) or mangling the already-stored settings.
 *
 * V1.5 mainboard: the mechanism and the fixed core are byte-identical to the
 * drdro-firmware-f4 baseline, but the application payload is a fresh layout (5
 * encoder scales instead of 4, plus analog-output / network / digital-input
 * fields), so the magic is bumped to "DRO2" — a DRO1 image from the old board can
 * never be mis-parsed as this layout, and vice versa. The append-only contract
 * restarts from this layout at SETTINGS_VERSION 1.
 *
 * Header-only: struct + CRC + validate + defaults + slot selection are pure (the
 * pick is pointer-based and host-testable); each project supplies its own flash
 * erase/program.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stddef.h>
#include "Bootloader.h"   /* SETTINGS_BASE, SETTINGS_SECTOR, BANK_COUNT, boot_mode_t */

#define SETTINGS_MAGIC    0x44524F32U   /* "DRO2" — V1.5 mainboard payload layout */
#define SETTINGS_VERSION  1U            /* informational; validity is length-based */
#define SETTINGS_SCALES   5U            /* must match the app's SCALES_COUNT */
#define SETTINGS_AOUTS    2U            /* GP8403 dual DAC channels */
#define SETTINGS_DINS     6U            /* ISO_IN1..6 opto inputs */

/* Two settings slots (ping-pong). A = sector 1, B = sector 2. */
#define SETTINGS_A_BASE    SETTINGS_BASE
#define SETTINGS_A_SECTOR  SETTINGS_SECTOR
#define SETTINGS_B_BASE    0x08008000U
#define SETTINGS_B_SECTOR  2U
#define SETTINGS_SLOT_BASE(n)    ((n) ? SETTINGS_B_BASE   : SETTINGS_A_BASE)
#define SETTINGS_SLOT_SECTOR(n)  ((n) ? SETTINGS_B_SECTOR : SETTINGS_A_SECTOR)
#define SETTINGS_SLOT_IMG(n)     ((const settings_t *)(uintptr_t)SETTINGS_SLOT_BASE(n))

/* FORWARD-COMPATIBILITY CONTRACT (critical — do not break):
 *   1. `crc` is ALWAYS the first field (offset 0). Its offset is frozen forever so any
 *      version — even one compiled before a field was added — can find and verify it.
 *   2. `used_size` records how many bytes the writer's struct occupied; the CRC covers
 *      bytes [4 .. used_size). Validation is therefore length-based, NOT sizeof-based: a
 *      binary can validate an image written by a *different* struct version (it CRCs the
 *      stored length straight out of flash). A SETTINGS_VERSION bump is informational only.
 *   3. NEW FIELDS ARE APPENDED AT THE END ONLY. Never insert or reorder. Existing offsets
 *      stay put, so an older/smaller reader still reads every field it knows correctly, and
 *      a newer reader defaults any field the stored image is too short to contain (see
 *      settings_load_one). This is what lets future updates add variables without mangling
 *      already-stored settings or breaking the (separately-flashed) bootloader. */
typedef struct {
  uint32_t crc;                         /* offset 0 — FROZEN. CRC32 over bytes [4 .. used_size) */
  uint32_t magic;                       /* offset 4 */
  uint16_t version;                     /* offset 8 — informational (validity is length-based) */
  uint16_t used_size;                   /* offset 10 — writer's sizeof(settings_t); CRC length */

  /* --- fixed core (board-control fields; offsets frozen, append-only like the payload) --- */
  uint32_t seq;                         /* monotonic write counter (ping-pong selection) */
  uint8_t  active_bank;                 /* 0|1 — which stored bank should run */
  uint8_t  loaded_bank;                 /* 0|1|0xFF — bank currently copied into Exec */
  uint16_t boot_mode;                   /* boot_mode_t: 0=app, 1=bootloader */
  uint32_t bank_crc[2];                 /* CRC32 of each bank's region (0 = unknown) */

  /* --- application payload (V1.5 mainboard) — APPEND AT THE END, never insert/reorder --- */
  int32_t  scale_num[SETTINGS_SCALES];      /* sync ratio numerator   (default 1)   */
  int32_t  scale_den[SETTINGS_SCALES];      /* sync ratio denominator (default 100) */
  uint16_t scale_sync[SETTINGS_SCALES];     /* sync enable            (default 0)   */
  uint16_t scale_filter[SETTINGS_SCALES];   /* TIM ICxF filter 0..15  (default 5)   */
  uint16_t scale_dir[SETTINGS_SCALES];      /* user direction flip    (default 0)   */
  uint16_t servo_mode;                      /* 0=off 1=sync/index 2=jog (default 0) */
  float    servo_max;                       /* max speed        (default 720)  */
  float    servo_acc;                       /* acceleration     (default 120)  */
  float    servo_jog;                       /* jog speed        (default 0)    */
  float    servo_index;                     /* index speed cap, 0 = use servo_max */
  uint16_t aout_raw[SETTINGS_AOUTS];        /* GP8403 12-bit codes (default 0 = 0 V) */
  uint8_t  net_dhcp;                        /* 1 = DHCP, 0 = static (default 1) */
  uint8_t  din_debounce_ms;                 /* input debounce window (default 5) */
  uint16_t net_port;                        /* TCP CLI port (default 5555; +1 = fw data) */
  uint8_t  net_ip[4];                       /* static address   (when net_dhcp = 0) */
  uint8_t  net_mask[4];                     /* static netmask */
  uint8_t  net_gw[4];                       /* static gateway */
  uint32_t com_baud;                        /* RS-485 baud, applied at next boot
                                               (default 115200; bootloader stays
                                               fixed at 115200 regardless) */
} settings_t;

/* Smallest image we'll accept: must at least reach through the fixed core. Bounds the
 * length-based CRC so a garbage used_size can't walk off the slot. */
#define SETTINGS_USED_MIN  ((uint16_t)offsetof(settings_t, scale_num))
#define SETTINGS_USED_MAX  ((uint16_t)4096U)

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) — table-less, tiny. */
static inline uint32_t settings_crc32(const void *data, uint32_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFU;
  while (len--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
/* CRC over everything after the crc word, for the writer-recorded used_size. Operates on the
 * raw image (which may be a flash pointer with used_size > sizeof here), so it reproduces the
 * writer's CRC exactly regardless of the reader's struct size. used_size must be set first. */
static inline uint32_t settings_compute_crc(const settings_t *s) {
  return settings_crc32((const uint8_t *)s + 4U, (uint32_t)s->used_size - 4U);
}
static inline int settings_valid(const settings_t *s) {
  if (s->magic != SETTINGS_MAGIC) return 0;
  if (s->used_size < SETTINGS_USED_MIN || s->used_size > SETTINGS_USED_MAX) return 0;
  return s->crc == settings_compute_crc(s);
}
static inline void settings_seal(settings_t *s) {   /* stamp magic/version/size + crc before write */
  s->magic = SETTINGS_MAGIC;
  s->version = SETTINGS_VERSION;
  s->used_size = (uint16_t)sizeof(*s);
  s->crc = settings_compute_crc(s);
}
static inline void settings_defaults(settings_t *s) {
  for (uint32_t i = 0; i < sizeof(*s); i++) ((uint8_t *)s)[i] = 0;
  s->boot_mode   = BOOT_MODE_APP;
  s->seq         = 0;
  s->active_bank = 0;
  s->loaded_bank = 0xFF;                /* unknown → force a copy on first boot */
  for (uint32_t i = 0; i < SETTINGS_SCALES; i++) {
    s->scale_num[i] = 1;
    s->scale_den[i] = 100;
    s->scale_sync[i] = 0;
    s->scale_filter[i] = 5;             /* ICxF 5 = f_DTS/2, N=8 → rejects pulses < 160 ns @ 100 MHz */
    s->scale_dir[i] = 0;
  }
  s->servo_max = 720.0f;
  s->servo_acc = 120.0f;
  s->servo_jog = 0.0f;
  s->servo_index = 0.0f;                /* 0 → ramp falls back to servo_max (see Ramps.c) */
  s->servo_mode = 0;
  for (uint32_t i = 0; i < SETTINGS_AOUTS; i++) s->aout_raw[i] = 0;
  s->net_dhcp = 1;
  s->din_debounce_ms = 5;
  s->net_port = 5555;
  for (int i = 0; i < 4; i++) { s->net_ip[i] = 0; s->net_gw[i] = 0; }
  s->net_mask[0] = 255; s->net_mask[1] = 255; s->net_mask[2] = 255; s->net_mask[3] = 0;
  s->com_baud = 115200;
  settings_seal(s);
}

/* Copy a valid flash image into *out, defaulting any field the image is too short to hold
 * (image from an older/smaller version) and ignoring any trailing bytes we don't know
 * (image from a newer/larger version). Append-only layout makes the shared prefix line up. */
static inline void settings_load_one(const settings_t *src, settings_t *out) {
  settings_defaults(out);               /* fields beyond src->used_size keep their defaults */
  uint32_t n = src->used_size;
  if (n > sizeof(*out)) n = sizeof(*out);
  for (uint32_t i = 0; i < n; i++) ((uint8_t *)out)[i] = ((const uint8_t *)src)[i];
  out->used_size = (uint16_t)sizeof(*out);   /* *out is a reader-sized struct; keep used_size honest
                                                (re-sealed on save). Avoids OOB if src was larger. */
}

/* Pure: choose the valid slot with the newest seq (signed diff handles wraparound).
 * Fills *out and returns 1; if neither is valid, fills defaults and returns 0.
 * Pointer-based so it is host-testable (the flash-reading wrappers below call it). */
static inline int settings_pick(const settings_t *a, const settings_t *b, settings_t *out) {
  int va = settings_valid(a), vb = settings_valid(b);
  if (va && vb) { settings_load_one(((int32_t)(b->seq - a->seq) >= 0) ? b : a, out); return 1; }
  if (va) { settings_load_one(a, out); return 1; }
  if (vb) { settings_load_one(b, out); return 1; }
  settings_defaults(out);
  return 0;
}

/* Load the newest valid slot from flash into *out (defaults if neither valid). */
static inline int settings_load(settings_t *out) {
  return settings_pick(SETTINGS_SLOT_IMG(0), SETTINGS_SLOT_IMG(1), out);
}

/* Which physical slot holds the newest valid image (0=A, 1=B), or -1 if none. */
static inline int settings_newest_slot(void) {
  int va = settings_valid(SETTINGS_SLOT_IMG(0)), vb = settings_valid(SETTINGS_SLOT_IMG(1));
  if (va && vb) return ((int32_t)(SETTINGS_SLOT_IMG(1)->seq - SETTINGS_SLOT_IMG(0)->seq) >= 0) ? 1 : 0;
  if (va) return 0;
  if (vb) return 1;
  return -1;
}

/* Prepare *s for writing: bump seq past the newest slot and seal; return the slot index
 * to program (the inactive one). Writers erase+program SETTINGS_SLOT_{SECTOR,BASE}(slot). */
static inline int settings_prepare(settings_t *s) {
  int newest = settings_newest_slot();
  uint32_t base_seq = (newest < 0) ? 0U : SETTINGS_SLOT_IMG(newest)->seq;
  s->seq = base_seq + 1U;
  settings_seal(s);
  return (newest < 0) ? 0 : (1 - newest);
}

#endif /* SETTINGS_H */

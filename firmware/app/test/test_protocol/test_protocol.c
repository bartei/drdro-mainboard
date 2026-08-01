/*
 * Native unit tests for the drDRO line protocol (transport-independent logic).
 * Ported from the drdro-firmware-f4 baseline; V1.5 changes: 5 scales, the
 * proto_io_t transport context, scales.dir, din/aout/net registry rows, the
 * dout command, and the fw.* network-update commands (stubbed).
 * HAL/CMSIS are mocked (test/mocks); responses are captured for assertions.
 * Run: pio test -e native
 */
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "Protocol.h"
#include "SettingsStore.h"
#include "Settings.h"
#include "FwUpdate.h"

/* ---- SettingsStore stubs (real impl is HW-only; no flash here) ----------- */
void    SettingsApply(rampsSharedData_t *s)      { (void)s; }
int     SettingsSave(const rampsSharedData_t *s) { (void)s; return 0; }
int     SettingsLoad(rampsSharedData_t *s)       { (void)s; return 1; }
int     SettingsBankSet(uint8_t bank)            { (void)bank; return 0; }
int     SettingsCommitBank(uint8_t bank, uint32_t crc) { (void)bank; (void)crc; return 0; }
uint8_t SettingsActiveBank(void)                 { return 0; }
void    EnterBootloader(void)                    { }   /* HW jump; stubbed for host tests */

/* ---- usart stubs (Rs485Send feeds the same capture as the HAL mock) ------ */
void MockUartCapture(const uint8_t *data, uint16_t size);
void Rs485Send(const uint8_t *data, uint16_t len) { MockUartCapture(data, len); }
void Rs485TxInit(void) { }

/* ---- Scales stub (encoder filter reprogram; captured for assertions) ----- */
static TIM_HandleTypeDef *filtHandle; static uint16_t filtValue; static int filtCalls;
void setScaleFilter(TIM_HandleTypeDef *t, uint16_t f) { filtHandle = t; filtValue = f; filtCalls++; }

/* ---- Aout stub (live-apply hook; captured) -------------------------------- */
static int aoutCalls; static uint8_t aoutCh; static uint16_t aoutVal; static int aoutRc = 0;
int AoutInit(void) { return 0; }
int AoutWrite(uint8_t ch, uint16_t raw) { aoutCalls++; aoutCh = ch; aoutVal = raw; return aoutRc; }
void AoutApply(const rampsSharedData_t *s) { (void)s; }

/* ---- FwUpdate stubs (network state machine is HW/socket-bound) ----------- */
static fw_state_t fwState = FW_IDLE;
static int fwBeginRc = 0, fwCommitRc = 0;
static uint8_t fwBeginBank; static uint32_t fwBeginSize, fwBeginCrc;
void        FwUpdateStart(rampsSharedData_t *s) { (void)s; }
int         FwUpdateBegin(uint8_t bank, uint32_t size, uint32_t crc) {
  fwBeginBank = bank; fwBeginSize = size; fwBeginCrc = crc;
  if (fwBeginRc == 0) fwState = FW_RECV;
  return fwBeginRc;
}
void        FwUpdateAbort(void)    { fwState = FW_IDLE; }
fw_state_t  FwUpdateState(void)    { return fwState; }
uint32_t    FwUpdateReceived(void) { return 123; }
const char *FwUpdateReason(void)   { return "crc"; }
uint8_t     FwUpdateBank(void)     { return fwBeginBank; }
int         FwUpdateCommit(uint32_t *crc) { if (fwCommitRc == 0 && crc) *crc = 0xA1B2C3D4U; return fwCommitRc; }

/* ---- GPIO capture (dout command) ------------------------------------------ */
GPIO_TypeDef MockGpioA, MockGpioB, MockGpioC;
static GPIO_TypeDef *gpioPort; static uint16_t gpioPin; static int gpioState = -1, gpioCalls;
void MockGpioWrite(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st) {
  gpioPort = port; gpioPin = pin; gpioState = (int)st; gpioCalls++;
}

/* ---- TX capture ------------------------------------------------------------ */
static char   cap[2048];
static size_t capLen;
void MockUartCapture(const uint8_t *d, uint16_t n) {
  if (capLen + n < sizeof(cap)) { memcpy(cap + capLen, d, n); capLen += n; cap[capLen] = 0; }
}
static void capReset(void) { capLen = 0; cap[0] = 0; }

/* Test transport: capture sink, no begin/end hooks (like the TCP path). */
static void testIoWrite(proto_io_t *io, const char *s, uint16_t n) {
  (void)io; MockUartCapture((const uint8_t *)s, n);
}
static proto_io_t testIo;

/* ---- fixtures ------------------------------------------------------------ */
static rampsSharedData_t shared;
static UART_HandleTypeDef huart;

void setUp(void)    { memset(&shared, 0, sizeof(shared)); ProtocolStart(&huart, &shared); capReset();
                      memset(&testIo, 0, sizeof(testIo)); testIo.writeFn = testIoWrite;
                      filtHandle = NULL; filtValue = 0; filtCalls = 0;
                      aoutCalls = 0; aoutRc = 0; fwState = FW_IDLE; fwBeginRc = 0; fwCommitRc = 0;
                      gpioPort = NULL; gpioPin = 0; gpioState = -1; gpioCalls = 0; }
void tearDown(void) {}

/* run a (mutable) command line through the parser on the test transport */
static proto_action_t run(const char *line) {
  char buf[160];
  strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  return ProtocolProcessLine(&testIo, buf);
}
static uint8_t xor8(const char *s) { uint8_t c = 0; while (*s) c ^= (uint8_t)*s++; return c; }

/* ---- tests --------------------------------------------------------------- */
static void test_version(void) {
  run("version");
  TEST_ASSERT_NOT_NULL(strstr(cap, "version="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "crc="));
}

static void test_help_lists_commands(void) {
  run("help");
  TEST_ASSERT_NOT_NULL(strstr(cap, "sta="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "set="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "settings="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "dout="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.begin="));
}

static void test_framing_ends_with_blank_line(void) {
  run("version");
  size_t L = strlen(cap);
  TEST_ASSERT_TRUE(L >= 2);
  TEST_ASSERT_EQUAL_STRING("\n\n", cap + L - 2);   /* crc line \n + terminator \n */
}

static void test_get_float_scalar(void) {
  shared.servo.maxSpeed = 720.0f;
  run("get servo.max");
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.max=720"));
}

static void test_set_float_scalar(void) {
  run("set servo.max 1000");
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, shared.servo.maxSpeed);
}

static void test_set_get_index_speed(void) {
  run("set servo.idx 200");
  TEST_ASSERT_EQUAL_FLOAT(200.0f, shared.servo.indexSpeed);
  run("get servo.idx");
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.idx=200"));
}

static void test_set_array_element(void) {
  run("set scales.num 2 7");
  TEST_ASSERT_EQUAL_INT32(7, shared.scales[2].syncRatioNum);
  TEST_ASSERT_EQUAL_INT32(0, shared.scales[0].syncRatioNum);   /* others untouched */
}

static void test_set_array_element_5th(void) {
  run("set scales.num 4 9");                    /* the V1.5 board's new 5th scale */
  TEST_ASSERT_EQUAL_INT32(9, shared.scales[4].syncRatioNum);
}

static void test_get_array_grouped(void) {
  for (int i = 0; i < SCALES_COUNT; i++) shared.scales[i].syncRatioNum = i + 1;
  run("get scales.num");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.num=1,2,3,4,5"));
}

static void test_sta(void) {
  shared.scales[0].position = 10; shared.scales[1].position = 20;
  shared.scales[0].speed = 5;
  shared.servo.currentSteps = 1234; shared.servo.currentSpeed = 7.5f;
  shared.servo.stepsToGo = -42; shared.fastData.servoMode = 2;
  shared.din.state = 0x25;
  run("sta");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.pos=10,20,0,0,0"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.speed=5,0,0,0,0"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.pos=1234"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.speed=7.5"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.tgt=-42"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.mode=2"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "din.state=37"));
}

static void test_settings_dumps_all(void) {
  run("settings");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.pos="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "servo.max="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "diag.cycles="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "net.addr="));
  TEST_ASSERT_NOT_NULL(strstr(cap, "aout.raw="));
}

static void test_error_unknown_command(void) {
  run("frobnicate");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=unknown command"));
}

static void test_error_unknown_variable(void) {
  run("get nope.var");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=unknown variable"));
}

static void test_error_readonly(void) {
  run("set scales.speed 0 5");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=read-only"));
  TEST_ASSERT_EQUAL_INT32(0, shared.scales[0].speed);
}

static void test_error_bad_index(void) {
  run("set scales.num 9 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=bad index"));
}

static void test_index_5_rejected(void) {
  run("set scales.num 5 1");                    /* 0..4 valid on this board */
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=bad index"));
}

static void test_error_out_of_range_u16(void) {
  run("set servo.mode 99999");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
}

/* ---- encoder input filter (scales.filt) ----------------------------------- */
static void test_set_filter_applies_live(void) {
  static TIM_HandleTypeDef tim2;
  shared.scales[1].timerHandle = &tim2;
  run("set scales.filt 1 9");
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_UINT16(9, shared.scales[1].filterValue);
  TEST_ASSERT_EQUAL_INT(1, filtCalls);                  /* reprogrammed the hardware... */
  TEST_ASSERT_EQUAL_PTR(&tim2, filtHandle);             /* ...on the right timer         */
  TEST_ASSERT_EQUAL_UINT16(9, filtValue);
}

static void test_filter_out_of_range_rejected(void) {
  run("set scales.filt 0 16");                          /* ICxF is 4-bit: 0..15 */
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
  TEST_ASSERT_EQUAL_UINT16(0, shared.scales[0].filterValue);
  TEST_ASSERT_EQUAL_INT(0, filtCalls);
}

static void test_filter_get_grouped(void) {
  for (int i = 0; i < SCALES_COUNT; i++) shared.scales[i].filterValue = (uint16_t)(i + 2);
  run("get scales.filt");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.filt=2,3,4,5,6"));
}

static void test_load_reapplies_filters(void) {
  run("load");
  TEST_ASSERT_EQUAL_INT(SCALES_COUNT, filtCalls);       /* one reprogram per scale */
}

/* ---- direction flip (scales.dir) ------------------------------------------ */
static void test_dir_set_get(void) {
  run("set scales.dir 4 1");
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_UINT16(1, shared.scales[4].dirInvert);
  capReset();
  run("get scales.dir");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.dir=0,0,0,0,1"));
}

static void test_dir_rejects_2(void) {
  run("set scales.dir 0 2");                            /* boolean flag */
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
  TEST_ASSERT_EQUAL_UINT16(0, shared.scales[0].dirInvert);
}

static void test_checksum_valid_accepted(void) {
  char line[64]; const char *body = "set servo.max 50";
  sprintf(line, "%s*%02X", body, xor8(body));
  run(line);
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, shared.servo.maxSpeed);
}

static void test_checksum_invalid_rejected(void) {
  char line[64]; const char *body = "set servo.max 50";
  sprintf(line, "%s*%02X", body, (uint8_t)(xor8(body) ^ 0xFF));   /* deliberately wrong */
  run(line);
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=bad checksum"));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, shared.servo.maxSpeed);           /* not applied */
}

static void test_checksum_bad_hex_rejected(void) {
  run("version*ZZ");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=bad checksum"));
}

static void test_empty_repeats_last(void) {
  shared.scales[0].position = 42;
  run("sta");
  capReset();
  run("");                                   /* empty line → repeat */
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.pos=42"));
}

static void test_empty_with_no_history(void) {
  run("");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=no previous command"));
}

static void test_repeat_is_per_transport(void) {
  run("version");                            /* history on the test transport */
  proto_io_t other; memset(&other, 0, sizeof(other)); other.writeFn = testIoWrite;
  capReset();
  char buf[4] = "";                          /* empty line on a FRESH transport */
  ProtocolProcessLine(&other, buf);
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=no previous command"));
}

static void test_feed_bytes_crlf_single_line(void) {
  const char *cmd = "version\r\n";
  for (const char *p = cmd; *p; ++p) ProtocolFeedByte((uint8_t)*p);
  TEST_ASSERT_TRUE(ProtocolLineReady());
  ProtocolService();                         /* UART path: Rs485Send stub captures */
  TEST_ASSERT_NOT_NULL(strstr(cap, "version="));
  TEST_ASSERT_FALSE(ProtocolLineReady());    /* \n after \r must not re-trigger */
}

static void test_activity_increments(void) {
  uint32_t before = ProtocolActivity();
  ProtocolFeedByte('s'); ProtocolFeedByte('t'); ProtocolFeedByte('a'); ProtocolFeedByte('\n');
  ProtocolService();
  TEST_ASSERT_EQUAL_UINT32(before + 1, ProtocolActivity());
}

/* ---- dual-bank / settings commands -------------------------------------- */
static void test_reset_acks_and_requests_handoff(void) {
  proto_action_t a = run("reset");
  TEST_ASSERT_NOT_NULL(strstr(cap, "reset=ok"));
  TEST_ASSERT_EQUAL_INT(PROTO_ACT_HANDOFF, a);
}
static void test_get_returns_no_action(void) {
  TEST_ASSERT_EQUAL_INT(PROTO_ACT_NONE, run("version"));
}
static void test_save_ok_when_idle(void) {
  shared.fastData.servoMode = 0;          /* motion stopped */
  run("save");
  TEST_ASSERT_NOT_NULL(strstr(cap, "save=ok"));
}
static void test_save_ok_during_motion(void) {
  shared.fastData.servoMode = 2;          /* jog: motion active — still allowed */
  run("save");                            /* RAM-resident ISR keeps stepping during the write */
  TEST_ASSERT_NOT_NULL(strstr(cap, "save=ok"));
}
static void test_bank_reports_active(void) {
  run("bank");
  TEST_ASSERT_NOT_NULL(strstr(cap, "bank.active="));
}
static void test_bank_select_ok(void) {
  shared.fastData.servoMode = 0;
  run("bank 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "bank.active=1"));
}
static void test_rollback_acks(void) {
  proto_action_t a = run("rollback");       /* stub SettingsActiveBank()==0 -> other=1 */
  TEST_ASSERT_NOT_NULL(strstr(cap, "rollback=1"));
  TEST_ASSERT_EQUAL_INT(PROTO_ACT_HANDOFF, a);
}

/* ---- dout (raw output override) ------------------------------------------ */
static void test_dout_sets_pin(void) {
  run("dout m2.step 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "dout.m2.step=1"));
  TEST_ASSERT_EQUAL_PTR(GPIOC, gpioPort);
  TEST_ASSERT_EQUAL_UINT16(GPIO_PIN_0, gpioPin);   /* M2_STEP = PC0 */
  TEST_ASSERT_EQUAL_INT(1, gpioState);
}
static void test_dout_unknown_output(void) {
  run("dout m9.step 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=unknown output"));
  TEST_ASSERT_EQUAL_INT(0, gpioCalls);
}
static void test_dout_bad_value(void) {
  run("dout ena 2");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=usage: dout"));
}
static void test_dout_motion_owned_refused_while_active(void) {
  shared.fastData.servoMode = 2;
  run("dout ena 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=motion active"));
  TEST_ASSERT_EQUAL_INT(0, gpioCalls);
  capReset();
  run("dout m2.dir 1");                     /* M2 is never motion-owned */
  TEST_ASSERT_NOT_NULL(strstr(cap, "dout.m2.dir=1"));
}
static void test_dout_motion_owned_allowed_when_idle(void) {
  shared.fastData.servoMode = 0;
  run("dout ena 1");
  TEST_ASSERT_NOT_NULL(strstr(cap, "dout.ena=1"));
  TEST_ASSERT_EQUAL_UINT16(GPIO_PIN_10, gpioPin);  /* M_ENA = PC10 */
}

/* ---- aout (GP8403 live-apply) --------------------------------------------- */
static void test_aout_set_applies_live(void) {
  run("set aout.raw 1 2048");
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_UINT16(2048, shared.aout.raw[1]);
  TEST_ASSERT_EQUAL_INT(1, aoutCalls);
  TEST_ASSERT_EQUAL_UINT8(1, aoutCh);
  TEST_ASSERT_EQUAL_UINT16(2048, aoutVal);
}
static void test_aout_out_of_range(void) {
  run("set aout.raw 0 4096");                /* 12-bit DAC */
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
  TEST_ASSERT_EQUAL_INT(0, aoutCalls);
}
static void test_aout_i2c_error_reported(void) {
  aoutRc = -1;
  run("set aout.raw 0 100");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=i2c"));
}

/* ---- net registry (IP4/MAC formatting + parsing) --------------------------- */
static void test_net_addr_formats(void) {
  shared.net.ip[0] = 10; shared.net.ip[1] = 1; shared.net.ip[2] = 2; shared.net.ip[3] = 105;
  run("get net.addr");
  TEST_ASSERT_NOT_NULL(strstr(cap, "net.addr=10.1.2.105"));
}
static void test_net_addr_readonly(void) {
  run("set net.addr 1.2.3.4");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=read-only"));
}
static void test_net_mac_formats(void) {
  uint8_t mac[6] = { 0x02, 0x00, 0x32, 0x17, 0x39, 0x32 };
  memcpy(shared.net.mac, mac, 6);
  run("get net.mac");
  TEST_ASSERT_NOT_NULL(strstr(cap, "net.mac=02:00:32:17:39:32"));
}
static void test_net_cfg_ip_roundtrip(void) {
  run("set net.cfg.ip 192.168.7.42");
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_UINT8(192, shared.net.cfgIp[0]);
  TEST_ASSERT_EQUAL_UINT8(42,  shared.net.cfgIp[3]);
  capReset();
  run("get net.cfg.ip");
  TEST_ASSERT_NOT_NULL(strstr(cap, "net.cfg.ip=192.168.7.42"));
}
static void test_net_cfg_ip_rejects_bad(void) {
  const char *bad[] = { "1.2.3.256", "1.2.3", "1.2.3.4.5", "a.b.c.d", "1..2.3", "1.2.3.4x" };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    capReset();
    char line[48];
    snprintf(line, sizeof(line), "set net.cfg.ip %s", bad[i]);
    run(line);
    TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
  }
}
static void test_net_dhcp_bounds(void) {
  run("set net.dhcp 1");
  TEST_ASSERT_EQUAL_UINT16(1, shared.net.dhcp);
  capReset();
  run("set net.dhcp 2");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=value out of range"));
}

/* ---- fw.* (network update; state machine stubbed) -------------------------- */
static void test_fw_begin_dispatch(void) {
  run("fw.begin 1 79052 DEADBEEF");
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.state=recv"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.port=5556"));
  TEST_ASSERT_EQUAL_UINT8(1, fwBeginBank);
  TEST_ASSERT_EQUAL_UINT32(79052, fwBeginSize);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, fwBeginCrc);
}
static void test_fw_begin_usage_errors(void) {
  const char *bad[] = { "fw.begin", "fw.begin 2 100 00000000", "fw.begin 0 0 00000000",
                        "fw.begin 0 100 XYZ", "fw.begin 0 100 1234" };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    capReset();
    char line[48];
    snprintf(line, sizeof(line), "%s", bad[i]);
    run(line);
    TEST_ASSERT_NOT_NULL(strstr(cap, "error=usage: fw.begin"));
  }
}
static void test_fw_begin_busy(void) {
  fwBeginRc = -2;
  run("fw.begin 0 100 00000000");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=busy"));
}
static void test_fw_status_reports(void) {
  fwState = FW_ERROR;
  run("fw.status");
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.state=error"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.recv=123"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.reason=crc"));
}
static void test_fw_commit_ok(void) {
  run("fw.commit");
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.commit=ok"));
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.crc=A1B2C3D4"));
}
static void test_fw_commit_not_ready(void) {
  fwCommitRc = -1;
  run("fw.commit");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=not ready"));
}
static void test_fw_abort(void) {
  fwState = FW_RECV;
  run("fw.abort");
  TEST_ASSERT_NOT_NULL(strstr(cap, "fw.state=idle"));
  TEST_ASSERT_EQUAL_INT(FW_IDLE, fwState);
}

/* ---- board capability report ------------------------------------------------ */
static void test_scales_count_reports_board_capability(void) {
  shared.scaleCount = SCALES_COUNT;          /* set by RampsStart on hardware */
  run("get scales.count");
  TEST_ASSERT_NOT_NULL(strstr(cap, "scales.count=5"));
  capReset();
  run("set scales.count 4");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=read-only"));
  TEST_ASSERT_EQUAL_UINT16(SCALES_COUNT, shared.scaleCount);
}

/* ---- diag stats ------------------------------------------------------------ */
static void test_diag_uptime_readonly(void) {
  /* 3 days, 4 h, 5 min, 42 s = 3*86400 + 4*3600 + 5*60 + 42 */
  shared.diag.uptimeS = 3U*86400U + 4U*3600U + 5U*60U + 42U;
  run("get diag.uptime");
  TEST_ASSERT_NOT_NULL(strstr(cap, "diag.uptime=3d04:05:42"));
  capReset();
  run("set diag.uptime 0");
  TEST_ASSERT_NOT_NULL(strstr(cap, "error=read-only"));
}
static void test_diag_uptime_zero(void) {
  shared.diag.uptimeS = 0;
  run("get diag.uptime");
  TEST_ASSERT_NOT_NULL(strstr(cap, "diag.uptime=0d00:00:00"));
}
static void test_diag_cycmax_resettable(void) {
  shared.diag.cyclesMax = 777;
  run("get diag.cycmax");
  TEST_ASSERT_NOT_NULL(strstr(cap, "diag.cycmax=777"));
  capReset();
  run("set diag.cycmax 0");                  /* re-arm the max-hold */
  TEST_ASSERT_NULL(strstr(cap, "error="));
  TEST_ASSERT_EQUAL_UINT32(0, shared.diag.cyclesMax);
}

/* ---- settings: CRC / validate / defaults / ping-pong pick (pure logic) ---- */
static void test_settings_crc32_vector(void) {
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, settings_crc32("123456789", 9));  /* CRC-32/ISO-HDLC */
}
static void test_settings_seal_validate(void) {
  settings_t s; settings_defaults(&s);
  TEST_ASSERT_TRUE(settings_valid(&s));
  ((uint8_t *)&s)[12] ^= 0xFF;                /* corrupt a byte inside the CRC-covered range */
  TEST_ASSERT_FALSE(settings_valid(&s));
}
static void test_settings_crc_is_first_field(void) {
  /* The forward-compat contract: crc at offset 0, then magic/version/used_size. */
  TEST_ASSERT_EQUAL_UINT(0, offsetof(settings_t, crc));
  TEST_ASSERT_EQUAL_UINT(4, offsetof(settings_t, magic));
  TEST_ASSERT_EQUAL_UINT(10, offsetof(settings_t, used_size));
}
static void test_settings_magic_is_dro2(void) {
  /* Fresh payload layout for the V1.5 board — never confusable with DRO1. */
  TEST_ASSERT_EQUAL_HEX32(0x44524F32, SETTINGS_MAGIC);
  TEST_ASSERT_EQUAL_UINT(5, SETTINGS_SCALES);
}
static void test_settings_forward_compat_shorter_image(void) {
  /* Image written by an OLDER firmware predating the net fields: used_size stops
   * before them and the CRC covers only that prefix. We must still validate,
   * preserve known fields, and DEFAULT the appended fields. */
  settings_t older, out;
  settings_defaults(&older);
  older.servo_max = 999.0f;
  older.net_port  = 0xAAAA;                   /* garbage in the not-yet-existing tail */
  older.net_dhcp  = 0xAA;
  older.used_size = (uint16_t)offsetof(settings_t, aout_raw);
  older.crc = settings_crc32((const uint8_t *)&older + 4U, (uint32_t)older.used_size - 4U);

  TEST_ASSERT_TRUE(settings_valid(&older));
  settings_load_one(&older, &out);
  TEST_ASSERT_EQUAL_FLOAT(999.0f, out.servo_max);    /* known field preserved */
  TEST_ASSERT_EQUAL_UINT16(5555, out.net_port);      /* appended fields defaulted */
  TEST_ASSERT_EQUAL_UINT8(1, out.net_dhcp);
  TEST_ASSERT_EQUAL_UINT16(sizeof(settings_t), out.used_size);
}
static void test_settings_forward_compat_longer_image(void) {
  /* Image from a NEWER firmware with extra trailing bytes our struct doesn't know: still
   * validate (length-based CRC over the stored bytes) and read our known fields. */
  uint8_t buf[sizeof(settings_t) + 8];
  settings_t *newer = (settings_t *)buf;
  settings_defaults(newer);
  newer->servo_index = 42.0f;
  for (unsigned i = 0; i < 8; i++) buf[sizeof(settings_t) + i] = (uint8_t)(0xA0 + i);
  newer->used_size = (uint16_t)(sizeof(settings_t) + 8U);
  newer->crc = settings_crc32(buf + 4U, (uint32_t)newer->used_size - 4U);

  TEST_ASSERT_TRUE(settings_valid(newer));
  settings_t out;
  settings_load_one(newer, &out);
  TEST_ASSERT_EQUAL_FLOAT(42.0f, out.servo_index);   /* known field read; unknown tail ignored */
  TEST_ASSERT_EQUAL_UINT16(sizeof(settings_t), out.used_size);
}
static void test_settings_defaults(void) {
  settings_t s; settings_defaults(&s);
  TEST_ASSERT_EQUAL_INT32(100, s.scale_den[0]);
  TEST_ASSERT_EQUAL_FLOAT(720.0f, s.servo_max);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.servo_index);
  TEST_ASSERT_EQUAL_UINT8(0xFF, s.loaded_bank);
  TEST_ASSERT_EQUAL_UINT32(0, s.seq);
  for (unsigned i = 0; i < SETTINGS_SCALES; i++) {
    TEST_ASSERT_EQUAL_UINT16(5, s.scale_filter[i]);
    TEST_ASSERT_EQUAL_UINT16(0, s.scale_dir[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(1, s.net_dhcp);
  TEST_ASSERT_EQUAL_UINT16(5555, s.net_port);
  TEST_ASSERT_EQUAL_UINT8(5, s.din_debounce_ms);
  TEST_ASSERT_EQUAL_UINT16(0, s.aout_raw[0]);
  TEST_ASSERT_EQUAL_UINT8(255, s.net_mask[0]);
  TEST_ASSERT_EQUAL_UINT8(0, s.net_mask[3]);
}
static void test_settings_pick_newest_seq(void) {
  settings_t a, b, out;
  settings_defaults(&a); a.active_bank = 0; a.seq = 5; settings_seal(&a);
  settings_defaults(&b); b.active_bank = 1; b.seq = 7; settings_seal(&b);
  TEST_ASSERT_EQUAL_INT(1, settings_pick(&a, &b, &out));
  TEST_ASSERT_EQUAL_UINT8(1, out.active_bank);            /* b has the newer seq */
  a.seq = 9; settings_seal(&a);
  TEST_ASSERT_EQUAL_INT(1, settings_pick(&a, &b, &out));
  TEST_ASSERT_EQUAL_UINT8(0, out.active_bank);            /* now a is newer */
}
static void test_settings_pick_validity_fallback(void) {
  settings_t a, b, out;
  settings_defaults(&a); a.active_bank = 0; settings_seal(&a);
  settings_defaults(&b); b.magic = 0;                     /* b invalid */
  TEST_ASSERT_EQUAL_INT(1, settings_pick(&a, &b, &out));
  TEST_ASSERT_EQUAL_UINT8(0, out.active_bank);            /* picks the only valid one */
  a.magic = 0;                                            /* now neither valid */
  TEST_ASSERT_EQUAL_INT(0, settings_pick(&a, &b, &out));
  TEST_ASSERT_TRUE(settings_valid(&out));                 /* defaults are returned, sealed */
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_version);
  RUN_TEST(test_help_lists_commands);
  RUN_TEST(test_framing_ends_with_blank_line);
  RUN_TEST(test_get_float_scalar);
  RUN_TEST(test_set_float_scalar);
  RUN_TEST(test_set_get_index_speed);
  RUN_TEST(test_set_array_element);
  RUN_TEST(test_set_array_element_5th);
  RUN_TEST(test_get_array_grouped);
  RUN_TEST(test_sta);
  RUN_TEST(test_settings_dumps_all);
  RUN_TEST(test_error_unknown_command);
  RUN_TEST(test_error_unknown_variable);
  RUN_TEST(test_error_readonly);
  RUN_TEST(test_error_bad_index);
  RUN_TEST(test_index_5_rejected);
  RUN_TEST(test_error_out_of_range_u16);
  RUN_TEST(test_set_filter_applies_live);
  RUN_TEST(test_filter_out_of_range_rejected);
  RUN_TEST(test_filter_get_grouped);
  RUN_TEST(test_load_reapplies_filters);
  RUN_TEST(test_dir_set_get);
  RUN_TEST(test_dir_rejects_2);
  RUN_TEST(test_checksum_valid_accepted);
  RUN_TEST(test_checksum_invalid_rejected);
  RUN_TEST(test_checksum_bad_hex_rejected);
  RUN_TEST(test_empty_repeats_last);
  RUN_TEST(test_empty_with_no_history);
  RUN_TEST(test_repeat_is_per_transport);
  RUN_TEST(test_feed_bytes_crlf_single_line);
  RUN_TEST(test_activity_increments);
  RUN_TEST(test_reset_acks_and_requests_handoff);
  RUN_TEST(test_get_returns_no_action);
  RUN_TEST(test_save_ok_when_idle);
  RUN_TEST(test_save_ok_during_motion);
  RUN_TEST(test_bank_reports_active);
  RUN_TEST(test_bank_select_ok);
  RUN_TEST(test_rollback_acks);
  RUN_TEST(test_dout_sets_pin);
  RUN_TEST(test_dout_unknown_output);
  RUN_TEST(test_dout_bad_value);
  RUN_TEST(test_dout_motion_owned_refused_while_active);
  RUN_TEST(test_dout_motion_owned_allowed_when_idle);
  RUN_TEST(test_aout_set_applies_live);
  RUN_TEST(test_aout_out_of_range);
  RUN_TEST(test_aout_i2c_error_reported);
  RUN_TEST(test_net_addr_formats);
  RUN_TEST(test_net_addr_readonly);
  RUN_TEST(test_net_mac_formats);
  RUN_TEST(test_net_cfg_ip_roundtrip);
  RUN_TEST(test_net_cfg_ip_rejects_bad);
  RUN_TEST(test_net_dhcp_bounds);
  RUN_TEST(test_fw_begin_dispatch);
  RUN_TEST(test_fw_begin_usage_errors);
  RUN_TEST(test_fw_begin_busy);
  RUN_TEST(test_fw_status_reports);
  RUN_TEST(test_fw_commit_ok);
  RUN_TEST(test_fw_commit_not_ready);
  RUN_TEST(test_fw_abort);
  RUN_TEST(test_scales_count_reports_board_capability);
  RUN_TEST(test_diag_uptime_readonly);
  RUN_TEST(test_diag_uptime_zero);
  RUN_TEST(test_diag_cycmax_resettable);
  RUN_TEST(test_settings_crc32_vector);
  RUN_TEST(test_settings_seal_validate);
  RUN_TEST(test_settings_crc_is_first_field);
  RUN_TEST(test_settings_magic_is_dro2);
  RUN_TEST(test_settings_forward_compat_shorter_image);
  RUN_TEST(test_settings_forward_compat_longer_image);
  RUN_TEST(test_settings_defaults);
  RUN_TEST(test_settings_pick_newest_seq);
  RUN_TEST(test_settings_pick_validity_fallback);
  return UNITY_END();
}

#include "Net.h"
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "cmsis_os.h"
#include "Ramps.h"   /* gBlinkCode + BlinkCode.h */

#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

#define DHCP_SOCKET_NUM   0U
#define SNIFF_SOCKET      0U   /* MACRAW self-test reuses socket 0 (DHCP not running yet) */

/* RIP_MSG is 236 + 312 = 548 bytes; the ioLibrary DHCP client receives whole
 * messages into this buffer, so it must be at least that. 1 KB leaves headroom. */
static uint8_t   dhcpBuffer[1024];

static NetState_t netState = NET_STATE_INIT;
static uint8_t    netMac[6];
static wiz_NetInfo netInfo;

/* Every state change also refreshes the LED blink code (BlinkCode.h): the LED
 * task in Ramps.c renders gBlinkCode; fine-grained state stays readable via
 * NetGetState(). Never clobbers a flash-error indication. */
static rampsSharedData_t *sShared = NULL;

static void SetNetState(NetState_t s)
{
  netState = s;
  if (sShared) sShared->net.state = (uint16_t)s;
  uint8_t code;
  switch (s) {
    case NET_STATE_LEASED:     code = BLINK_APP;       break;
    case NET_STATE_CHIP_ERROR: code = BLINK_NET_ERROR; break;
    default:                   code = BLINK_NET_DOWN;  break;
  }
  if (gBlinkCode != BLINK_ERR_FLASH) gBlinkCode = code;
}

static osMutexId_t wizMutex;
static osThreadId_t netTaskHandle;

/* ------------------------------------------------------------------------- */
/* Bring-up diagnostics                                                       */
/*                                                                            */
/* Raw W5500 register snapshot, refreshed every loop. This exists to be read   */
/* over SWD during bring-up (the board's only console is RS-485, which needs a  */
/* transceiver on the other end). The important one is shar_readback: reading   */
/* VERSIONR only proves SPI READS work, whereas reading back a MAC we WROTE     */
/* proves the write path too. If writes were failing, DHCP would sit sending    */
/* nothing forever and look exactly like "no DHCP server".                     */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint8_t  versionr;         /* expect 0x04                                  */
  uint8_t  phycfgr;          /* bit0 LNK, bit1 SPD, bit2 DPX                 */
  uint8_t  shar_readback[6]; /* must equal netMac -> proves SPI writes work   */
  uint8_t  sipr[4];          /* source IP as the chip currently holds it      */
  uint8_t  sock0_mr;         /* expect 0x02 (Sn_MR_UDP) while DHCP runs       */
  uint8_t  sock0_sr;         /* expect 0x22 (SOCK_UDP)                        */
  uint8_t  writeOk;          /* 1 = shar_readback matched netMac              */
  uint8_t  phyModeIdx;       /* index into phyModes[] currently applied        */
  uint8_t  selfTestDone;     /* 1 once the per-mode RX sniff has finished      */
  uint8_t  sniffPhy[5];      /* PHYCFGR observed in each mode                  */
  uint8_t  sniffLink[5];     /* link up? per mode                              */
  uint16_t sniffRx[5];       /* max RX_RSR seen per mode (>0 => RX path alive) */
  uint8_t  sniffSr[5];       /* Sn_SR after opening MACRAW; 0x42 = SOCK_MACRAW  */
  int8_t   sniffOpenRc[5];   /* socket() return code (<0 = open failed)        */
  uint16_t sock0_txfsr;      /* free TX buffer                                */
  uint16_t sock0_rxrsr;      /* pending RX bytes (>0 = a reply arrived!)      */
  uint32_t loops;            /* proves the task is alive                      */
  uint32_t discoverCycles;   /* how many DHCP sessions have been started      */
} NetDiag_t;

static volatile NetDiag_t netDiag;

static void NetDiagRefresh(void)
{
  uint8_t shar[6];
  uint8_t sipr[4];

  netDiag.versionr = getVERSIONR();
  netDiag.phycfgr  = getPHYCFGR();

  getSHAR(shar);
  memcpy((void *)netDiag.shar_readback, shar, 6);
  netDiag.writeOk = (memcmp(shar, netMac, 6) == 0) ? 1U : 0U;

  getSIPR(sipr);
  memcpy((void *)netDiag.sipr, sipr, 4);

  netDiag.sock0_mr    = getSn_MR(DHCP_SOCKET_NUM);
  netDiag.sock0_sr    = getSn_SR(DHCP_SOCKET_NUM);
  netDiag.sock0_txfsr = getSn_TX_FSR(DHCP_SOCKET_NUM);
  netDiag.sock0_rxrsr = getSn_RX_RSR(DHCP_SOCKET_NUM);
  netDiag.loops++;
}

NetState_t NetGetState(void) { return netState; }

void NetGetMac(uint8_t mac[6]) { memcpy(mac, netMac, 6); }

void NetGetAddress(uint8_t ip[4], uint8_t sn[4], uint8_t gw[4], uint8_t dns[4])
{
  if (ip)  memcpy(ip,  netInfo.ip,  4);
  if (sn)  memcpy(sn,  netInfo.sn,  4);
  if (gw)  memcpy(gw,  netInfo.gw,  4);
  if (dns) memcpy(dns, netInfo.dns, 4);
}

/* ------------------------------------------------------------------------- */
/* ioLibrary callbacks — SPI2 + software chip-select                          */
/* ------------------------------------------------------------------------- */

/* Chip select. Held LOW across a whole variable-data-mode transaction: the
 * W5500 frames [addr_hi][addr_lo][control][data...] by SCSn, not per byte. */
static void w5500_cs_select(void)
{
  HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_RESET);
}

static void w5500_cs_deselect(void)
{
  HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_SET);
}

/* On STM32F4, HAL_SPI_Receive() in master/2-line mode internally defers to
 * HAL_SPI_TransmitReceive(pData, pData, ...) to generate the clock, so the
 * buffer contents are shifted out as dummy bytes. The W5500 ignores MOSI during
 * the data phase of a read, so that is harmless. */
static uint8_t w5500_spi_readbyte(void)
{
  uint8_t rx = 0xFFU;
  HAL_SPI_Receive(&hspi2, &rx, 1U, 100U);
  return rx;
}

static void w5500_spi_writebyte(uint8_t wb)
{
  HAL_SPI_Transmit(&hspi2, &wb, 1U, 100U);
}

static void w5500_spi_readburst(uint8_t *pBuf, uint16_t len)
{
  HAL_SPI_Receive(&hspi2, pBuf, len, 1000U);
}

static void w5500_spi_writeburst(uint8_t *pBuf, uint16_t len)
{
  HAL_SPI_Transmit(&hspi2, pBuf, len, 1000U);
}

/* Critical section around each wizchip access. A recursive mutex (not a
 * taskENTER_CRITICAL) because a single access can be a multi-hundred-byte SPI
 * burst, and masking interrupts for that long would wreck the 1 kHz tick and,
 * later, the motion ISR. Falls through to a no-op before the kernel is running
 * (NetInit runs inside the task, but keep this safe either way). */
static void w5500_cris_enter(void)
{
  if (wizMutex != NULL && osKernelGetState() == osKernelRunning)
  {
    osMutexAcquire(wizMutex, osWaitForever);
  }
}

static void w5500_cris_exit(void)
{
  if (wizMutex != NULL && osKernelGetState() == osKernelRunning)
  {
    osMutexRelease(wizMutex);
  }
}

/* ------------------------------------------------------------------------- */
/* PHY mode sweep                                                             */
/*                                                                            */
/* The W5500 is 10/100 only. On first bring-up we observed PHYCFGR = 0xBB:     */
/* link up at 100 Mbps but HALF duplex, which is the signature of auto-        */
/* negotiation failing and the PHY falling back to parallel detect. In that    */
/* state frames can be transmitted (the switch sees them) while incoming ones  */
/* are corrupted and dropped on CRC, which looks exactly like "no DHCP server". */
/*                                                                            */
/* Rather than assume the link partner's capabilities, walk through the modes  */
/* on each failed DHCP cycle. 10BASE-T half duplex is last and is by far the   */
/* most tolerant of marginal cabling/magnetics, so reaching a lease only at    */
/* 10M is itself a strong diagnosis of a physical-layer problem.               */
/* ------------------------------------------------------------------------- */
typedef struct {
  const char *name;
  uint8_t     opmdc;   /* pre-shifted PHYCFGR_OPMDC_* value */
} PhyMode_t;

/* Order matters. 10M half is FIRST because it is the empirically known-good
 * setting for this W5500 on this network: auto-negotiation here produces a
 * 100 Mbps link that carries no DHCP traffic, and the same was seen previously
 * on separate W5500 breakout boards, where only a 10 Mbps link ever completed
 * DHCP. 10BASE-T is half duplex by definition — forcing 10M FULL drops the link
 * outright on most switches, which is why it is last. */
static const PhyMode_t phyModes[] = {
  { "10M half",      PHYCFGR_OPMDC_10H  },
  { "auto-neg all",  PHYCFGR_OPMDC_ALLA },
  { "100M full",     PHYCFGR_OPMDC_100F },
  { "100M half",     PHYCFGR_OPMDC_100H },
  { "10M full",      PHYCFGR_OPMDC_10F  },
};
#define PHY_MODE_COUNT (sizeof(phyModes) / sizeof(phyModes[0]))

static uint8_t phyModeIdx;

/**
 * Apply phyModes[idx] and wait briefly for the link to come back up.
 *
 * PHYCFGR is written directly rather than via wizphy_setphyconf(). That helper
 * calls wizphy_reset(), which does getPHYCFGR() *after* asserting the reset bit
 * and then writes the read-back value out again — so the OPMDC bits it just
 * programmed are clobbered by whatever the PHY reports while held in reset
 * (in practice the PMODE strap default, 111 = auto-neg all). Measured on this
 * board: requesting "10M half" read back as PHYCFGR 0xFB, i.e. OPMDC 111. The
 * forced modes silently never applied.
 *
 * The correct sequence is two blind writes with no read-back in between:
 * OPMD|OPMDC with RST low (hold PHY in reset), then the same value with RST
 * high to release it. A PHY-only reset leaves MAC/socket registers intact, so
 * the DHCP socket setup survives.
 */
static void ApplyPhyMode(uint8_t idx)
{
  uint8_t base;
  int i;

  idx %= PHY_MODE_COUNT;
  phyModeIdx = idx;
  netDiag.phyModeIdx = idx;

  /* OPMD=1 => take the operating mode from OPMDC rather than the strap pins. */
  base = (uint8_t)(PHYCFGR_OPMD | phyModes[idx].opmdc);

  setPHYCFGR(base);                      /* bit7 RST = 0 -> PHY held in reset */
  osDelay(5);
  setPHYCFGR((uint8_t)(base | 0x80U));   /* release reset, keep OPMD + OPMDC   */
  osDelay(50);

  /* Give the PHY time to re-establish (auto-neg can take ~2 s). */
  for (i = 0; i < 30; i++)
  {
    osDelay(100);
    if (wizphy_getphylink() == PHY_LINK_ON)
    {
      break;
    }
  }

  {
    uint8_t p = getPHYCFGR();
    DebugPrintf("PHY: mode '%s' -> PHYCFGR 0x%02X (link %s, %s, %s)\r\n",
                phyModes[idx].name, p,
                (p & PHYCFGR_LNK_ON) ? "up" : "DOWN",
                (p & PHYCFGR_SPD_100) ? "100M" : "10M",
                (p & PHYCFGR_DPX_FULL) ? "full" : "half");
  }
}

/**
 * Open socket 0 in MACRAW and watch RX_RSR for `ms`, returning the largest
 * value seen. MACRAW hands the host EVERY frame on the wire, so on any live
 * network this must go non-zero within a second or two (ARP/mDNS/NetBIOS
 * broadcast traffic alone guarantees it).
 *
 * This is the test that separates the two remaining explanations for
 * "transmits fine, never receives":
 *   result > 0  -> the RX path works; the fault is specific to DHCP
 *   result == 0 -> nothing is being received at all, i.e. the receive side of
 *                  the PHY/magnetics/pair is dead in this PHY mode
 * The buffer is deliberately not drained: filling and stalling is fine for a
 * yes/no answer and keeps this free of MACRAW framing concerns.
 */
__attribute__((unused))
static uint16_t MacrawSniff(uint32_t ms, int8_t *openRc, uint8_t *sr)
{
  uint16_t peak = 0U;
  uint32_t start;
  int8_t   rc;

  close(SNIFF_SOCKET);
  rc = socket(SNIFF_SOCKET, Sn_MR_MACRAW, 0, 0);
  *openRc = rc;
  *sr = getSn_SR(SNIFF_SOCKET);   /* 0x42 = SOCK_MACRAW means it really opened */
  if (rc != (int8_t)SNIFF_SOCKET)
  {
    return 0U;
  }

  start = osKernelGetTickCount();
  while ((osKernelGetTickCount() - start) < ms)
  {
    uint16_t rsr = getSn_RX_RSR(SNIFF_SOCKET);
    if (rsr > peak)
    {
      peak = rsr;
    }
    osDelay(20);
  }

  close(SNIFF_SOCKET);
  return peak;
}

/**
 * Walk every PHY mode and measure whether anything is received in each.
 * Bounded (~8 s per mode) and runs once at boot before DHCP starts.
 */
__attribute__((unused))
static void NetSelfTest(void)
{
  uint8_t i;

  DebugPrint("SELFTEST: sniffing each PHY mode for received traffic...\r\n");

  for (i = 0; i < PHY_MODE_COUNT; i++)
  {
    uint8_t phy, sr = 0U;
    uint16_t rx;
    int8_t rc = 0;

    ApplyPhyMode(i);
    phy = getPHYCFGR();
    rx  = MacrawSniff(4000U, &rc, &sr);

    netDiag.sniffPhy[i]    = phy;
    netDiag.sniffLink[i]   = (phy & PHYCFGR_LNK_ON) ? 1U : 0U;
    netDiag.sniffRx[i]     = rx;
    netDiag.sniffSr[i]     = sr;
    netDiag.sniffOpenRc[i] = rc;

    DebugPrintf("SELFTEST: %-13s PHYCFGR=0x%02X link=%s sock=0x%02X rc=%d rx=%u\r\n",
                phyModes[i].name, phy,
                (phy & PHYCFGR_LNK_ON) ? "up" : "down", sr, (int)rc, (unsigned)rx);
  }

  netDiag.selfTestDone = 1U;
}

/* ------------------------------------------------------------------------- */
/* Bring-up helpers                                                          */
/* ------------------------------------------------------------------------- */

/**
 * Derive a stable, locally-administered MAC from the STM32 96-bit unique device
 * ID. The board has no MAC EEPROM, so this keeps the address constant across
 * reboots (DHCP reservations keep working) and unique between boards.
 * First octet 0x02 = locally administered, unicast.
 */
static void MacFromDeviceId(uint8_t mac[6])
{
  uint32_t uid0 = HAL_GetUIDw0();
  uint32_t uid1 = HAL_GetUIDw1();
  uint32_t uid2 = HAL_GetUIDw2();

  mac[0] = 0x02U;
  mac[1] = (uint8_t)(uid0 >> 24);
  mac[2] = (uint8_t)(uid1 >> 16);
  mac[3] = (uint8_t)(uid1);
  mac[4] = (uint8_t)(uid2 >> 8);
  mac[5] = (uint8_t)(uid2);
}

/**
 * Hardware reset via nRST (PC7). The W5500 needs nRST low for at least 500 ns
 * and then ~50 ms before the internal PLL and PHY are usable; the delays here
 * are generous because this runs once at boot.
 */
static void W5500_HardReset(void)
{
  w5500_cs_deselect();

  HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
  osDelay(2);
  HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
  osDelay(60);
}

/**
 * Register the callbacks, reset the chip, verify it answers, and size the socket
 * buffers. Returns 0 on success.
 */
static int NetChipInit(void)
{
  /* 8 sockets, 2 KB TX + 2 KB RX each = the W5500's full 16 KB + 16 KB. */
  uint8_t txsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
  uint8_t rxsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
  uint8_t version;

  reg_wizchip_cris_cbfunc(w5500_cris_enter, w5500_cris_exit);
  reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
  reg_wizchip_spi_cbfunc(w5500_spi_readbyte, w5500_spi_writebyte);
  reg_wizchip_spiburst_cbfunc(w5500_spi_readburst, w5500_spi_writeburst);

  W5500_HardReset();

  /* VERSIONR on the W5500 always reads 0x04. Anything else means the SPI link
   * is not working (wiring, CS framing, clock polarity, or the chip is held in
   * reset) — worth failing loudly rather than waiting on DHCP forever. */
  version = getVERSIONR();
  if (version != 0x04U)
  {
    DebugPrintf("W5500: bad VERSIONR 0x%02X (expected 0x04) - check SPI2/CS\r\n",
                version);
    return -1;
  }

  if (wizchip_init(txsize, rxsize) != 0)
  {
    DebugPrint("W5500: wizchip_init failed (socket buffer sizing)\r\n");
    return -1;
  }

  /* The MAC must be in SHAR before DHCP_init(): the DHCP client reads it back
   * out of the chip to build chaddr / the client identifier option. */
  MacFromDeviceId(netMac);
  memset(&netInfo, 0, sizeof(netInfo));
  memcpy(netInfo.mac, netMac, 6);
  netInfo.dhcp = NETINFO_DHCP;
  wizchip_setnetinfo(&netInfo);

  DebugPrintf("W5500: ready, MAC %02X:%02X:%02X:%02X:%02X:%02X\r\n",
              netMac[0], netMac[1], netMac[2], netMac[3], netMac[4], netMac[5]);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* DHCP callbacks                                                            */
/* ------------------------------------------------------------------------- */

static void ReportLease(const char *what)
{
  wizchip_getnetinfo(&netInfo);
  DebugPrintf("DHCP %s: IP %u.%u.%u.%u/%u.%u.%u.%u GW %u.%u.%u.%u DNS %u.%u.%u.%u\r\n",
              what,
              netInfo.ip[0], netInfo.ip[1], netInfo.ip[2], netInfo.ip[3],
              netInfo.sn[0], netInfo.sn[1], netInfo.sn[2], netInfo.sn[3],
              netInfo.gw[0], netInfo.gw[1], netInfo.gw[2], netInfo.gw[3],
              netInfo.dns[0], netInfo.dns[1], netInfo.dns[2], netInfo.dns[3]);
}

/* Registering these hooks REPLACES the ioLibrary's default_ip_assign /
 * default_ip_update, which are what normally push the leased address into the
 * chip — so they must program it here, or the W5500 would never learn its IP. */
static void ApplyLease(void)
{
  getIPfromDHCP(netInfo.ip);
  getGWfromDHCP(netInfo.gw);
  getSNfromDHCP(netInfo.sn);
  getDNSfromDHCP(netInfo.dns);
  memcpy(netInfo.mac, netMac, 6);
  netInfo.dhcp = NETINFO_DHCP;
  wizchip_setnetinfo(&netInfo);

  if (sShared) {
    memcpy(sShared->net.ip,   netInfo.ip, 4);
    memcpy(sShared->net.mask, netInfo.sn, 4);
    memcpy(sShared->net.gw,   netInfo.gw, 4);
  }
  SetNetState(NET_STATE_LEASED);
}

static void OnIpAssigned(void)
{
  ApplyLease();
  ReportLease("assigned");
}

static void OnIpUpdated(void)
{
  ApplyLease();
  ReportLease("renewed");
}

static void OnIpConflict(void)
{
  DebugPrint("DHCP: address conflict detected\r\n");
  SetNetState(NET_STATE_DHCP_FAILED);
}

/* ------------------------------------------------------------------------- */
/* Net task                                                                  */
/* ------------------------------------------------------------------------- */

static void NetTask(void *argument)
{
  (void)argument;

  uint32_t lastSecondTick;
  uint32_t retryAtTick = 0U;   /* backoff deadline after a failed DHCP cycle */
  uint32_t linkDownSince = 0U; /* for the link-down PHY watchdog below        */
  int      dhcpStarted = 0;
  int8_t   linkWasUp = 0;

  MX_SPI2_Init();

  if (NetChipInit() != 0)
  {
    SetNetState(NET_STATE_CHIP_ERROR);
    /* Nothing further is possible without the chip; park here so the LED keeps
     * signalling the fault. */
    for (;;)
    {
      osDelay(1000);
    }
  }

  /* One-shot bring-up diagnostic: which PHY mode, if any, actually receives.
   * Costs ~40 s, so it is opt-in (-D NET_SELFTEST=1). It is what identified the
   * 100BASE-TX receive failure; keep it available for the next board/switch. */
#if defined(NET_SELFTEST) && (NET_SELFTEST != 0)
  NetSelfTest();
#endif

  /* Start from a known PHY configuration rather than whatever the strap pins
   * left behind — this also logs the negotiated speed/duplex. */
  ApplyPhyMode(0U);

  SetNetState(NET_STATE_NO_LINK);
  lastSecondTick = osKernelGetTickCount();

  /* Static-IP mode (settings: net.dhcp = 0): no DHCP at all — apply the
   * persisted address whenever the link is up. Mirror the MAC either way. */
  uint8_t staticMode = (sShared && sShared->net.dhcp == 0U) ? 1U : 0U;
  uint8_t staticApplied = 0U;
  if (sShared) memcpy(sShared->net.mac, netMac, 6);

  for (;;)
  {
    int8_t linkUp;

    NetDiagRefresh();
    linkUp = (wizphy_getphylink() == PHY_LINK_ON) ? 1 : 0;

    if (staticMode)
    {
      if (linkUp && !staticApplied)
      {
        memcpy(netInfo.mac, netMac, 6);
        memcpy(netInfo.ip, sShared->net.cfgIp, 4);
        memcpy(netInfo.sn, sShared->net.cfgMask, 4);
        memcpy(netInfo.gw, sShared->net.cfgGw, 4);
        memcpy(netInfo.dns, sShared->net.cfgGw, 4);
        netInfo.dhcp = NETINFO_STATIC;
        wizchip_setnetinfo(&netInfo);
        memcpy(sShared->net.ip,   netInfo.ip, 4);
        memcpy(sShared->net.mask, netInfo.sn, 4);
        memcpy(sShared->net.gw,   netInfo.gw, 4);
        staticApplied = 1U;
        DebugPrintf("ETH: static %u.%u.%u.%u\r\n",
                    netInfo.ip[0], netInfo.ip[1], netInfo.ip[2], netInfo.ip[3]);
        SetNetState(NET_STATE_LEASED);
      }
      else if (!linkUp && staticApplied)
      {
        staticApplied = 0U;
        SetNetState(NET_STATE_NO_LINK);
      }
      else if (!linkUp)
      {
        /* keep the PHY-mode watchdog below alive for static mode too */
      }
      /* fall through: link logging + PHY watchdog run for both modes; the
       * DHCP machinery below is gated on !staticMode. */
    }

    if (linkUp != linkWasUp)
    {
      DebugPrintf("ETH: link %s\r\n", linkUp ? "up" : "down");
      linkWasUp = linkUp;

      if (!linkUp)
      {
        /* Drop the lease state: on re-plug we want a fresh DISCOVER rather than
         * to keep advertising an address that may belong to another subnet. */
        if (dhcpStarted)
        {
          DHCP_stop();
          dhcpStarted = 0;
        }
        retryAtTick = 0U;
        SetNetState(NET_STATE_NO_LINK);
      }
    }

    /* PHY watchdog. A manually forced mode the link partner cannot do (10M FULL
     * on most switches) leaves the link permanently down — and with no link the
     * DHCP path never runs, so it never "fails" and the sweep would never
     * advance. Without this the state machine wedges in the first bad mode. */
    if (!linkUp)
    {
      uint32_t now = osKernelGetTickCount();
      if (linkDownSince == 0U)
      {
        linkDownSince = now;
      }
      else if ((now - linkDownSince) > 6000U)
      {
        DebugPrintf("PHY: no link for 6s in mode '%s', advancing\r\n",
                    phyModes[phyModeIdx].name);
        ApplyPhyMode(phyModeIdx + 1U);
        linkDownSince = osKernelGetTickCount();
      }
    }
    else
    {
      linkDownSince = 0U;
    }

    if (linkUp && !staticMode)
    {
      /* After a failed cycle, hold off before discovering again: it keeps the
       * NET_STATE_DHCP_FAILED LED pattern visible and avoids flooding the
       * network with back-to-back DISCOVERs. */
      if (!dhcpStarted && retryAtTick != 0U &&
          (int32_t)(osKernelGetTickCount() - retryAtTick) < 0)
      {
        /* still backing off */
      }
      else if (!dhcpStarted)
      {
        reg_dhcp_cbfunc(OnIpAssigned, OnIpUpdated, OnIpConflict);
        DHCP_init(DHCP_SOCKET_NUM, dhcpBuffer);
        dhcpStarted = 1;
        retryAtTick = 0U;
        SetNetState(NET_STATE_DHCP_WAIT);
        netDiag.discoverCycles++;
        DebugPrintf("DHCP: discovering (PHY '%s')...\r\n",
                    phyModes[phyModeIdx].name);
      }

      switch (dhcpStarted ? DHCP_run() : (uint8_t)DHCP_RUNNING)
      {
        case DHCP_IP_ASSIGN:
        case DHCP_IP_CHANGED:
          /* Handled in the callbacks above (state + logging). */
          break;

        case DHCP_IP_LEASED:
          SetNetState(NET_STATE_LEASED);
          break;

        case DHCP_FAILED:
          DebugPrint("DHCP: no offer, backing off before retry\r\n");
          SetNetState(NET_STATE_DHCP_FAILED);
          /* Tear the session down and re-arm after the backoff, so a DHCP
           * server that appears later is still picked up. */
          DHCP_stop();
          dhcpStarted = 0;
          retryAtTick = osKernelGetTickCount() + 5000U;
          if (retryAtTick == 0U)
          {
            retryAtTick = 1U;   /* 0 is the "no backoff pending" sentinel */
          }
          /* Try the next PHY mode before the next DISCOVER: if the link is
           * negotiated wrongly (or the switch/cable can't do 100BASE-TX), no
           * amount of retrying at the current setting will ever get an offer. */
          ApplyPhyMode(phyModeIdx + 1U);
          break;

        case DHCP_RUNNING:
        default:
          if (dhcpStarted && netState != NET_STATE_LEASED)
          {
            SetNetState(NET_STATE_DHCP_WAIT);
          }
          break;
      }
    }

    /* The DHCP client's lease/retry timers are driven by a 1 Hz tick. */
    if ((osKernelGetTickCount() - lastSecondTick) >= 1000U)
    {
      lastSecondTick += 1000U;
      DHCP_time_handler();
    }

    osDelay(50);
  }
}

void NetStart(rampsSharedData_t *shared)
{
  sShared = shared;
  const osThreadAttr_t netTaskAttr = {
    .name       = "net",
    .stack_size = 1024U * 4U,   /* DHCP builds a 548-byte message on the stack path */
    .priority   = (osPriority_t)osPriorityNormal,
  };

  wizMutex = osMutexNew(&(const osMutexAttr_t){
    .name = "wizchip",
    .attr_bits = osMutexRecursive | osMutexPrioInherit,
  });

  netTaskHandle = osThreadNew(NetTask, NULL, &netTaskAttr);
  if (netTaskHandle == NULL)
  {
    Error_Handler();
  }
}

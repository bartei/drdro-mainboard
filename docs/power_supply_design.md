# Power supply — 24V → 5V buck stage

## Decision

Replace the LM2596-5.0 with a **TI LMR33630ADDAR** synchronous buck converter
(HSOIC-8 PowerPAD, adjustable output, 400 kHz).

## Why this part

- **3.8–36 V input range** — large margin above the 24 V nominal rail. The
  board is a motion-control mainboard (rotary/servo), so the 24 V bus likely
  carries switching noise from motor drivers; 36 V abs-max headroom absorbs
  that without derating the part right at its ceiling (unlike parts rated
  only to 24–28 V).
- **Synchronous topology** (no catch diode) — peak efficiency >95%, far above
  the LM2596's typical ~75–85% at 24V→5V/2A.
- **3 A rated device vs. 2 A max load** — ~33% continuous-current headroom,
  keeps thermal/transient margin comfortable.
- **400 kHz switching** allows an 8–10 µH inductor and ceramic-only output
  caps, vs. the LM2596's typical 33–100 µH inductor + electrolytic bulk caps.
  Meaningful footprint reduction.
- **Internally compensated** — no external compensation network, minimal
  part count.
- Confirmed **not deprecated**, in active production.

## Stock verification (checked 2026-07-03, JLCPCB SMT-assembly inventory — verify again before ordering, this changes daily)

| Part | Role | JLCPCB # | Stock | Class | Notes |
|---|---|---|---|---|---|
| LMR33630ADDAR (TI) | Buck controller | C841384 | 9,995 | Extended | 3.8–36V in, 3A, 400kHz, HSOIC-8 PowerPAD |
| CYA0630-10UH (SHOU HAN) | Power inductor | C5189958 | 66,769 | Extended | 10µH, 4A rated / 5.5A sat, 71mΩ DCR, 7.2×6.6mm shielded |
| TPS54331DR (TI) | Fallback controller (alt) | C9865 | 94,107 | Extended | Non-sync alt, see below |

Other LMR33630 variants checked and rejected for this design:
- `LMR33630BDDAR` (1.4MHz) — C544370, only 1,835 units. Usable if board space
  later forces a smaller inductor, but thinner stock margin.
- `LMR33630CDDA` (2.1MHz) — out of stock at time of check.
- `MP2315`/`MP2338` (MPS) — not carried on LCSC/JLCPCB under the genuine MPS
  brand, or Vin range (≤24-28V) leaves no headroom over the 24V nominal rail.
- CENKER `CKCS5040-8.2uH/M` inductor — only 410 units and flagged "not
  recommended for new designs" on JLCPCB. Avoided.

## Design equations used (LMR33630 datasheet §9.2.2, SNVSAN3F)

- FB divider: `RFBB = RFBT / (VOUT/VREF − 1)`, VREF = 1.0 V nominal.
  Base point RFBT = 100 kΩ → RFBB = 24.9 kΩ for VOUT = 5 V (both 1%). This
  was superseded by the adjustable divider below, kept here for reference.
- Adjustable divider (4.8–5.2V range): solved the two endpoint equations
  simultaneously against a 2kΩ trim span (`RFBB_min = 9.5·Rtrim`,
  `RFBT = 4.2·RFBB_min`) → **RFBT = 80.6kΩ**, **RFBB1 (fixed) = 19.1kΩ**,
  **RV1 = 2kΩ 25-turn trimmer** (Bourns 3296W-1-202LF, THT SIP-3, JLCPCB
  C83684, 12,135 in stock). Trim at 0Ω → ~5.22V; trim at 2kΩ → ~4.82V —
  brackets the requested 4.8–5.2V with margin so setpoints aren't at the
  pot's end-stops. ~16mV/turn resolution. See wiring step 6 below for how
  the trimmer is wired as a rheostat (not a bare 3-terminal divider) so a
  worn/lost wiper contact can't float FB.
- Inductor: `L = (VIN−VOUT)/(fSW·K·IOUTmax) · VOUT/VIN`, K = 0.3 ripple
  factor, **IOUTmax = 3 A (device rating, not the 2A application max — the
  datasheet explicitly says to use the device max when the app's max load is
  much smaller)** → L ≈ 8.1 µH. Nearest well-stocked standard value used:
  **10 µH** (6.8 µH is also acceptable; floor per eq. 5 is
  `LMIN ≥ 0.28·VOUT/fSW` ≈ 3.5 µH).
- Output cap: datasheet's quick-reference table gives 4×22µF for
  VOUT=5V/400kHz row regardless of Vin.
- Input cap: "preferably rated for twice the max input voltage" → 50V-rated
  ceramics for a 24V nominal rail. Two 10µF in parallel (rather than a single
  10µF) to offset DC-bias capacitance derating at 24V bias.

## Wiring (HSOIC-8 PowerPAD pinout: 1 PGND, 2 VIN, 3 EN, 4 PG, 5 FB, 6 VCC, 7 BOOT, 8 SW, thermal pad = AGND)

1. 24V rail → **CIN bank**: 2×10µF 50V X7R ceramic (1210 case) in parallel +
   **CHF** 220nF 50V X7R, both directly across VIN(2)/PGND(1), placed as
   close to the pins as physically possible.
2. VIN(2) → **EN(3)** tied directly together. No external UVLO divider
   needed — regulator follows the 24V rail. (If upstream sequencing ever
   needs a delayed enable, add the RENT/RENB divider per datasheet §9.2.2.10;
   not needed for this design.)
3. **SW(8)** → **L1** (10µH, CYA0630-10UH) → VOUT node.
4. **BOOT(7)** ← 100nF ceramic (≥10V rating, e.g. 25V X7R) → **SW(8)**.
5. VOUT node → **COUT bank**: 4×22µF 16V X5R/X7R ceramic (1210 case) to AGND,
   plus one small 100nF placed at the point of load for HF noise
   suppression.
6. VOUT node → **RFBT** (80.6kΩ 1%) → **FB(5)**. From FB(5) → **RFBB1**
   (19.1kΩ 1%, fixed) → **RV1 pin 1** (2kΩ 25-turn trimmer, 3296W-1-202LF).
   **RV1 pin 2 (wiper) shorted to pin 3**; pin 3 → AGND. Wiring the trimmer
   as a 2-terminal rheostat (rather than tapping FB off the wiper directly)
   means a worn/lost wiper contact reverts to the trimmer's fixed end-to-end
   resistance instead of floating FB open (FB must never float per
   datasheet). Route the FB trace short and away from the SW node/inductor.
   Dab of varnish/thread-lock on the trimmer screw post-calibration
   recommended given this board sees motion/vibration.
7. **VCC(6)** → **CVCC** 1µF 16V ceramic → AGND. Do not load VCC with
   external circuitry.
8. **PG(4)**: open-drain flag, optional. Leave floating if unused, or pull to
   VCC(6) through 100kΩ to get a "5V good" signal into an MCU GPIO.
9. **PGND(1)** and the exposed **thermal pad (AGND)** both return to the same
   ground pour immediately beside the IC. Solder the thermal pad to the
   ground plane with a via array — dissipation is low (<1W at 2A/~95%
   efficiency) so this is mostly a formality, not a hard thermal requirement.
10. Keep the VIN→SW→L1→COUT→PGND hot loop physically tight (standard buck
    layout practice) to minimize EMI radiation.

`CFF` (feed-forward cap across RFBT) is **not populated** — only needed at
RFBT > 100kΩ or specific transient-response tuning, not required here.

## Alternates for stock resilience

- **LMR33630BDDAR** (1.4MHz, C544370) — same pin/BOM structure, smaller
  inductor (2.2–4.7µH) if board space later becomes the dominant constraint.
  Re-check stock before switching (1,835 units at last check).
- **TPS54331DR** (non-synchronous, C9865, 94,107 in stock) — very deep stock
  fallback if the whole LMR33630 family craters. Needs an external Schottky
  catch diode (e.g. SS34) between SW and PGND, slightly lower efficiency,
  and only 28V abs-max input (less headroom than the 36V LMR33630).

## RS485 transceiver — PART CHANGE (2026-07-09)
**U5 SIT3088ETK (DFN-8) is out of stock at JLCPCB.** Replacement: **SP3485EN-L/TR
(JLCPCB C8963, SOIC-8, 3.3V, 10Mbps)** — ~191k in stock, ~$0.30. Pin-identical to
the SIT3088 (`1 RO, 2 RE̅, 3 DE, 4 DI, 5 GND, 6 A, 7 B, 8 VCC`), so it's an
electrical drop-in: PA11 GPIO-DE, R9/R10 680Ω bias, R12 120Ω term all unchanged.
Only action: change the U5 footprint DFN-8→SOIC-8 (SOIC-8 has no exposed pad, so
the "EP→GND" layout item disappears). 10Mbps SP3485 matches the design's USART1
ceiling. Alt if ever needed: MAX3485ESA+T (C18148, ~11k stock, ~5× the price).
Re-confirm stock at order time (changes daily). Historical auto-direction analysis
below is superseded by the GPIO-DE topology (see round 3/4 notes).

## RS485 auto-direction circuit — analysis + max speed (2026-07-04)

**Topology (correct).** U5 = SIT3088ETK (14 Mbps half-duplex, VCC=3V3).
DE and RE# are tied together (net $5N277). Q1 (BC847B): base ← R1(470Ω) ←
UART1_TX; emitter → GND; collector → $5N277. R2(2.2k) pulls $5N277 → 3V3.
DI is tied directly to UART1_TX.
- TX idle/mark (high): Q1 on → $5N277 low → DE=0 (driver off), RE#=0 (RX on).
  Bus mark held **passively by the fail-safe bias** (R57/R63/R62 = 120Ω).
- TX space (low): Q1 off → R2 pulls $5N277 high → DE=1 (driver on), drives space.
This is the classic TX-sensing single-NPN auto-DE. It works. Note DE=RE# are
tied, so the (DE=0,RE#=1) shutdown state is never entered — no shutdown glitch.

**Reconciles the earlier "120Ω bias too heavy" flag:** because the driver is
OFF during every mark, the mark level is set entirely by the bias network, so
strong 120Ω bias is actually *wanted* here (fast, solid mark recovery). The
SP3088E driver is rated to drive 54Ω (VOD 1.5V min), so it still makes
compliant space levels against the 120Ω bias+term load. **Keep 120Ω if
staying with auto-direction;** only raise R57/R63 to ~560–680Ω if switching to
an always-enabled / GPIO-DE driver.

**Speed limiter = the auto-direction loop delay, not the transceiver.**
Enable delay (TX-fall → driver actively driving the space):
- Q1 storage time: R1=470Ω over-drives the base (Ib≈5.5mA to sink only
  ~1.5mA of R2 current → forced β≈0.27, *deep* saturation) → long storage
  time, ~200–400ns. This is the dominant, least-controlled term.
- R2·C node charge to VIH(2.0V): 2.2k × ~22pF ≈ 45ns.
- SIT3088 driver enable: tens of ns (fast part).
→ total enable ≈ **~300–450ns**. Disable (Q1 on, active pulldown) is faster,
~100–150ns. Every UART frame's start bit is a space needing this enable, so
the enable delay must stay a small fraction of one bit time.

**Practical max signaling rate with the circuit as drawn:**
| Baud | Bit time | Enable delay as % of bit | Verdict |
|---|---|---|---|
| 115.2 kbps | 8.68µs | ~4% | rock solid |
| 250 kbps | 4.0µs | ~9% | reliable |
| 500 kbps | 2.0µs | ~18% | marginal (short/clean links only) |
| ≥1 Mbps | ≤1.0µs | ≥35% | not reliable |
So: **use ≤250 kbps; treat 500 kbps as the marginal ceiling; don't rely on
≥1 Mbps.** The 14 Mbps transceiver is ~30–50× underutilized.

**To go faster (if needed):**
1. Cheap: raise R1 to ~4.7–10k (cuts base over-drive → shorter storage time),
   add a ~100pF speed-up cap across R1, optionally a 10k Q1 base-emitter
   resistor. Pushes the marginal ceiling toward ~1 Mbps.
2. Best: drop the transistor and drive DE/RE# from a spare STM32 GPIO
   (assert before TX, deassert on the USART TC "transmission complete" flag).
   Deterministic, unlocks the transceiver's full rate. Note the F411 USART is
   the older IP **without** the hardware Driver-Enable (DEM) feature of
   F0/F3/F7/L4 — so it's a firmware-toggled GPIO, timed off TC.

## Out of scope

Assumes the upstream 24V input already has reverse-polarity/TVS protection
elsewhere on the board; this stage starts at the local 24V net. (Confirmed
present in the netlist review below: D2 TVS + U3 PPTC fuse + D3 series
Schottky, dedicated to this regulator's input.)

## Netlist review findings (2026-07-03, Netlist_Schematic1_2026-07-03.net)

Reviewed with the netlist-review skill (151 components, 137 nets, no
board-wide dangling nets) plus manual trace of every net touching U1.

**🔴 Critical — U1 PGND (pin 1) not tied to system ground.** Net `$2N4625`
has only 2 members: `U1-1 (PGND)` and `R3-1`. It never joins the main `GND`
net (95 pins — EP pad, all cap grounds, etc). The switching-current return
path is open; the regulator cannot function as wired. Fix: wire PGND
directly to GND, at the same local ground node as the input/output cap
returns and the EP pad.

**🔴 Critical (same area) — R3 (100kΩ, C149504) wired VCC→PGND instead of
VCC→PG.** Value matches the datasheet's own PG-pullup recommendation exactly,
but it landed on the wrong net — PG (U1 pin 4) is completely floating
elsewhere. Either move R3's second leg to PG(4) and route PG onward (if the
power-good flag is wanted, e.g. to the STM32), or delete R3 (harmless as
wired, but does nothing useful). Doesn't substitute for the PGND fix above.

**🟠 Verify — FB trim range shifted down from target.** As-built: RFBT =
R31(51k)+R29(30k) = 81k; RFBB = R32(2k trim)+R30(20k) = 20–22k (20k basic
substituted for the exact 19.1k calculated earlier). Range computes to
~4.68–5.05V vs. the ~4.82–5.22V originally targeted. Range isn't critical per
requirements, so left as-is.
CORRECTION (round 3): an earlier version of this note said "swap R30 to
22–24k to recenter nearer 5.00V" — that is BACKWARDS. VOUT = VREF·(1+RFBT/RFBB),
so INCREASING the bottom resistor RFBB LOWERS VOUT. To recenter HIGHER, make
RFBB smaller or RFBT larger. See round-3 findings for the correct fix.

### Round 4 (2026-07-04) — round-3 fixes verified + speed/SPI questions

Verified `Netlist_Schematic1_2026-07-04 (1).net` (142 comp / 130 nets, **no
dangling nets**). All round-3 items resolved:
- ✅ **FB divider on target:** top now R31(51k)+R4(33k)=84k, bottom R2(20k)+
  R32(2k trim) → **VOUT 4.82–5.20V** (exactly the requested window).
- ✅ **UART_DE pull-down added:** R1=10k to GND (defaults to RX at power-up).
- ✅ **Activity-LED circuit removed cleanly** (no orphaned nets).
- Buck (U1: PGND+EP on GND, SW→L) and RS485 (U5, bias R28/R30=680Ω, term
  R33=120Ω) intact after tool re-annotation.

**RS485 max speed with GPIO-DE circuit.** Driver now enabled for the whole
frame (mark + space actively driven), so the auto-direction per-bit limit is
gone. Bounds: transceiver SIT3088E = 14 Mbps; USART1 is on APB2 (100 MHz) →
max baud = fPCLK2/8 = **12.5 Mbps** (OVER8). 10 Mbps → USARTDIV=1.25, exactly
representable → **0% baud error**. So 10 Mbps is clean; ceiling ~12.5 Mbps
(MCU), then 14 Mbps (transceiver). Requires SYSCLK=100MHz + OVER8=1; DE
deassert via USART TC ISR (adds ~2–3 bit-times turnaround, not a throughput
limit). Cable ≤ ~10–12 m at 10 Mbps, both ends terminated 120Ω.
Confirmed pin mapping (ST DS Table 8): PA15=USART1_TX, PA10=USART1_RX,
PA11(DE)=plain GPIO (also USART1_CTS, unused as such).

**Spare SPI bus: SPI2 fully available.** PB12/PB13/PB14/PB15 all free =
SPI2 NSS/SCK/MISO/MOSI (AF5). SPI2 on APB1 (50 MHz) → up to 25 Mbit/s SCK.
16 free GPIOs total (PC0-3, PC6-9, PC14-15, PA2-3, PB12-15) for CS lines /
more devices. Handy alts among them: PC6/PC7=USART6, PA2/PA3=USART2.

**Still-open from round 2 (not yet addressed):**
- 🟠 VCAP C16 still **10µF** — change to 2.2µF (STM32F411 spec).
- 🟠 Decoupling coverage (~8 caps for the logic ICs) — verify 100nF/IC.
- 🔍 U5 SIT3088 EP pad → GND; crystal load caps vs CL.
- ⚪ ISO_IN pull-ups (internal), U16 unused RX inputs, NRST cap.

### Round 3 (2026-07-04) — RS485 DE→MCU + RJ45 removal (Netlist_..._2026-07-04.net)

Reviewed the updated export (147 comp / 138 nets).

**✅ DE control moved to MCU (correct):** Q1/R1/R2 auto-direction removed;
`UART_DE` = PA11 (U10-44) + U5 DE(3) + RE#(2). DI→UART_TX (PA15), RO→UART_RX
(PA10). This is the 10 Mbps-capable topology. USART1 is on APB2 (100MHz) so
it can clock 10 Mbps; drive PA11 high before TX, deassert in the USART TC ISR.

**✅ RS485 bias/termination correctly re-done** (coupled change): old 120Ω
R57/R62/R63 replaced by R28=680Ω (A→3V3 bias), R30=680Ω (B→GND bias),
R33=120Ω (A–B termination). Right values for an always-driven bus. Note
designators reshuffled by the tool (RS485 bias now R28/R30/R33; FB bottom
resistor is now R27).

**✅ RJ45 (SERIAL) removed** — intended by user (SWD still available on the
5-pin SWD header; RS485 now on CN2).

**🔴 FB divider now can't reach 5.0V — regression from the backwards advice
above.** Bottom resistor was changed 20k → **R27=22k**, so RFBB = 22–24k with
RFBT=81k → VOUT ≈ **4.38–4.68V**. The rail can no longer be set to 5.0V.
Fix (bottom resistor must go DOWN, not up):
- Minimal (1 change): R27 22k → **20k** → 4.68–5.05V (reaches ~5.0V).
- On-target (2 changes): R27 → **20k** AND R29 30k → **33k** (RFBT=84k) →
  **4.82–5.20V**, exactly the requested window. All basic 0805 (20k C4328,
  33k C17633, 51k C17737).

**🟠 UART_DE has no pull-down** — add ~10k from UART_DE to GND so DE defaults
to receive at power-up before firmware drives PA11 (prevents bus contention
during boot/reset).

**⚪ Activity-LED circuit orphaned by the net rename** (user already fixing):
4 dangling nets — UART1_TX(R4), UART1_RX(R66), TX_LED(R64), RX_LED(R65).
Q2/Q3 (MMBT5401) TX/RX activity drivers lost their signal when UART1_*→UART_*.
If keeping the LEDs: move R4→UART_TX, R66→UART_RX and reconnect TX/RX_LED to
the indicators. If dropping them: delete Q2, Q3, R4, R64, R65, R66.

### Round 2 (2026-07-04) — corrections verified + full-board sweep

**Both prior criticals confirmed fixed:** U1 PGND (pin 1) now on `GND`; EP
pad on `GND`; R3 (100k) now VCC→PG (pin 4), PG no longer floating. Power
stage otherwise unchanged and correct.

Extended the review to the whole board (151 comp / 137 nets, no dangling
nets). Architecture: STM32F411 + 3× AM26LV32 diff-encoder receivers + 10×
NC7SZ157 muxes (TTL-vs-differential select per channel via TTL_SEL) + 6×
TLP2309 opto-isolated inputs + SIT3088 RS485 (auto-direction via Q1) + ULN2003
STEP/DIR/ENA driver. No new critical/board-killer defects. Open items:

- 🟠 **VCAP cap C16 = 10µF — STM32F411 spec is 2.2µF.** Oversized cap on the
  internal-LDO pin risks stability/startup margin. Change to 2.2µF X7R.
- 🟠 **3V3 decoupling looks light:** ~8 bypass caps for ~15 logic ICs (MCU 4
  rails + 3 receivers + 10 muxes + RS485). Ensure a local 100nF per IC,
  especially the 10 NC7SZ157 muxes; verify on layout.
- 🔍 **RS485 fail-safe bias R57/R63 = 120Ω** (same as the 120Ω termination
  R62). Standard bias is 560–680Ω; at 120Ω the idle bias draws ~9mA and the
  combined bias+termination load (~48Ω with a far-end term) may exceed the
  SIT3088 driver, dropping VOD below the 1.5V RS485 minimum. Keep R62=120Ω;
  raise R57/R63 to ~560–680Ω unless the driver spec proves 120Ω is safe.
- 🔍 **U5 (SIT3088, DFN-8) exposed pad** — confirm EP tied to GND on layout
  (invisible in netlist).
- 🔍 **Crystal load caps C18/C19 = 12pF** — verify against X1's CL (≈10pF
  target → ~9–12pF caps depending on stray); currently plausible.
- ⚪ **ISO_IN1–6 have no external pull-up** — rely on STM32 internal pull-ups
  (TLP2309 open-collector output; indicator LED cathode on each node). Works
  if firmware enables pull-ups; add explicit 10k to 3V3 for a defined
  power-up state + noise immunity if desired.
- ⚪ **ULN2003 STEP/DIR/ENA outputs** pull up through an activity LED + 1.5k
  to 5V (weak, LED-drop-reduced high). Fine for opto/high-Z stepper-driver
  inputs; verify adequate for the specific external driver's input.
- ⚪ **U16 unused receiver channels 3&4** inputs left floating — tie to a
  defined level to avoid output oscillation (minor).
- ⚪ **NRST** has pull-up (R89 1.5k) + steering diodes but no 100nF to GND —
  ST-recommended but optional given internal filter.
- ⚪ **Designators:** U6 (inductor) and U3 (PPTC fuse) use "U"; conventionally
  L / F. Cosmetic (BOM/silkscreen clarity).

**✅ Confirmed correct:** R32 wired as a 2-terminal rheostat (pins 2+3
shorted, fail-safe against a worn wiper); input caps 2×10µF+220nF/50V;
output caps 4×22µF/25V; CBOOT 100nF BOOT–SW; CVCC 1µF VCC–GND; EN tied to
VIN; inductor (designated **U6** — cosmetic, consider renaming to
L-prefix) correctly bridges SW→5V; downstream AMS1117-3.3 (U2) correctly
wired off the 5V rail; D3/D2/U3 input-protection front-end (Schottky +
TVS + PPTC fuse) present and appropriately sized — and confirmed to be
input reverse-polarity protection, **not** a freewheeling diode for the
inductor (not needed — LMR33630 is synchronous).

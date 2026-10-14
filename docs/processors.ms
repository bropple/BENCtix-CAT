.\" The processors in the Cat S22 Flip. Build the PDF with:
.\"   TZ=UTC groff -k -t -ms -Tpdf -P-pletter docs/processors.ms > docs/processors.pdf
.nr PS 10
.nr VS 12
.nr PO 0.75i
.nr LL 7i
.nr HM 0.6i
.nr FM 0.6i
.ds CH
.ds FAM H
.na
.hy 0
.TL
How many computers are in a Cat S22 Flip
.LP
About eleven, depending on how strict you are about what counts as one.
The main chip (a Qualcomm QM215, a variant of the MSM8917) holds about
seven separate processors, each running its own firmware; four more sit
outside it.
.SH
Inside the main chip
.sp 0.4
.TS
expand box tab(@);
lb lb lb lb
l lw(1.4i) lw(2.1i) lw(2.6i).
#@Processor@Runs@Where it shows up in BENCtix-CAT
_
1@T{
4 \(mu Cortex-A53
T}@T{
Linux; TrustZone runs on the same cores in a hidden secure mode
T}@T{
\(lqThe phone\(rq
T}
_
2@T{
Modem (Hexagon DSP)
T}@T{
Qualcomm's own operating system and the LTE stack
T}@T{
Calls, texts, IMS, and GPS: the location engine lives here (memshare, D078)
T}
_
3@T{
Audio and sensor DSP (Hexagon)
T}@T{
Audio processing and the sensor hub
T}@T{
The accelerometer, pressure and proximity sensors all go through it; it
never slept until an audio clock was released
T}
_
4@T{
Pronto (a small ARM core)
T}@T{
The WiFi and Bluetooth firmware
T}@T{
WoWLAN; it woke the phone for Tailscale's packets (D082)
T}
_
5@T{
RPM (Cortex-M3)
T}@T{
Power management: votes on clocks and voltages for everyone else
T}@T{
The deep-sleep (vlow, vmin) counts in the battery log come from its memory
T}
_
6@T{
Venus
T}@T{
Video encoding and decoding firmware
T}@T{
The \(lqvenus\(rq interrupt
T}
_
7@T{
GPU (Adreno 308)
T}@T{
Shaders, plus a small command processor running its own microcode
T}@T{
The browser's renderer and recovering from a GPU reset (D077)
T}
.TE
.SH
Outside the main chip
.sp 0.4
.TS
expand box tab(@);
lb lb lb
l lw(1.6i) lw(4.6i).
#@Part@Why it counts
_
8@T{
Touch controller (chipsemi, 0-002e)
T}@T{
A small microcontroller; the kernel log shows its \(lqfirmware 8020\(rq at
every wake
T}
_
9@T{
eMMC storage
T}@T{
Its own controller, usually an ARM core, manages the flash (wear
levelling, bad blocks)
T}
_
10@T{
SD card
T}@T{
The same: a controller in the card
T}
_
11@T{
SIM card
T}@T{
A real smartcard computer: a CPU, memory, and usually a small Java
operating system
T}
.TE
.SH
Not counted
.LP
The PMIC (power chip) has the voltage-mode battery gauge and the charger
logic, but it is fixed hardware, not a CPU. The same goes for the camera
image pipeline, the crypto engine and the USB-C controller. The motion and
light sensors are just sensors; the audio DSP does their thinking.
.SH
Who is in charge
.LP
Linux is only one of these, and not the one in charge. TrustZone, the RPM
and the modem each run code Linux can't see or override; Linux mostly
negotiates with them by passing messages. That is why so much of the port
has been about message channels (SMD, QMI, QRTR) and firmware rather than
drivers in the usual sense.

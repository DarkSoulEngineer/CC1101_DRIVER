CC1101 driver power and PA tables
=================================

All power figures on this page are TI's nominal conducted values from the
433 MHz PA table (DN013) for the given PATABLE bytes. They are not measured
on this hardware, and PA register settings alone establish neither
radiated power (ERP) nor regulatory compliance; measure the assembled
system and follow the local frequency, power, and airtime rules before
transmitting.

PATABLE and PA index
--------------------

The CC1101 shapes amplitude modulation through the 8-entry PATABLE and the
FREND0.PA_POWER index. OOK and ASK share modulation code 3: entry 0 is the
low (or off) level and entry 1 the high level, with PA_POWER = 1 so the
modulator switches between them. Constant-envelope 2-FSK/GFSK use entry 0
with PA_POWER = 0. ``cc1101_set_tx_power()`` preserves this layout when
changing the level, and ``cc1101_apply_profile_locked()`` writes the whole
table deterministically per profile.

Normal profile levels (default build)
-------------------------------------

.. list-table::
   :header-rows: 1

   * - Profile
     - PATABLE entries used
     - Nominal conducted level
   * - OOK
     - ``{0x00, 0xC8}``, PA index 1
     - off / +7 dBm
   * - ASK
     - ``{0x34, 0xC8}``, PA index 1
     - -10 dBm / +7 dBm (~17 dB contrast)
   * - 2-FSK
     - ``{0xC8, 0x00}``, PA index 0
     - +7 dBm constant envelope
   * - GFSK
     - ``{0xC8, 0x00}``, PA index 0
     - +7 dBm constant envelope

``CC1101_DEFAULT_PA_433_MHZ`` is ``0xC8``. ASK uses two nonzero levels so
the low symbol is a real attenuated carrier rather than OOK-style off
keying. Both FSK profiles write ``DEVIATN = 0x35``, which for the 26 MHz
crystal quantizes to about +/-20.63 kHz deviation; the amplitude profiles
ignore deviation, but the register is still written on every transition
so the delta stays deterministic.

Kconfig bench options
---------------------

``components/cc1101/Kconfig`` defines two explicit close-range bench
options. Both default to ``n``; the default build keeps the normal levels
above. They select lower PA tables for short bench links only. They are
not automatic power adaptation, and enabling them changes only the PA
levels: no automatic transmissions or retries are added.

``CONFIG_CC1101_OOK_CLOSE_RANGE_BENCH``
   OOK only: PATABLE ``{0x00, 0x12}``, where ``0x12``
   (``CC1101_CLOSE_RANGE_PA_433_MHZ``) is TI's nominal -30 dBm conducted
   setting. ASK keeps ``{0x34, 0xC8}`` and FSK/GFSK keep ``0xC8``.

``CONFIG_CC1101_ALL_PROFILES_CLOSE_RANGE_BENCH``
   All profiles at the nominal low-power class: OOK ``{0x00, 0x12}``,
   ASK ``{0x12, 0x34}`` (nominal -30/-10 dBm), 2-FSK/GFSK
   ``{0x12, 0x00}`` (nominal -30 dBm). This option overrides the OOK-only
   option when both are set.

The header resolves these into ``CC1101_OOK_PA_433_MHZ``,
``CC1101_ASK_LOW_PA_433_MHZ``, ``CC1101_ASK_HIGH_PA_433_MHZ``, and
``CC1101_FSK_PA_433_MHZ`` at compile time, and both initialization
(``cc1101_config_checked()``) and profile reapply use the same macros, so
the selected levels stay consistent across resets and transitions.

Register quantization
---------------------

The radio runs from a 26 MHz crystal and every requested value is
quantized into register fields; requested and actual values can differ:

* Data rate: ``cc1101_set_datarate()`` solves ``DRATE = (256 + DRATE_M) *
  2^DRATE_E * f_xosc / 2^28``. A request of 38400 baud lands on
  MDMCFG4/3 = ``CA/83`` (DRATE_E = 10, DRATE_M = 0x83), which is
  approximately 38.383 kbps, not exactly 38400.
* Frequency: ``cc1101_set_frequency()`` writes ``FREQ = freq_hz * 2^16 /
  26 MHz`` truncated to 24 bits. The 433.92 MHz preset words
  ``10/A7/62`` decode to about 433.9199 MHz, a step of roughly 397 Hz.
* Deviation: ``DEVIATN = 0x35`` (exponent 3, mantissa 3) gives
  ``f_xosc / 2^17 * 8 * (1 + 3/8)`` = about 20.63 kHz, not a round
  20 kHz.
* RX bandwidth: MDMCFG4 = ``0xCA`` selects 101.5625 kHz from the
  channel-bandwidth table; arbitrary bandwidths are not selectable.

The profile readback (``cc1101_profile_readback_locked()``) reports the
register values actually in hardware, which is what the application
should display as the applied configuration rather than echoing the
request.

References
----------

* `TI DN013: 433 MHz PA tables <https://www.ti.com/lit/an/swra151a/swra151a.pdf>`_
* `CC1101 datasheet (SWRS061I) <https://www.ti.com/lit/ds/symlink/cc1101.pdf>`_
CC1101 driver API reference
===========================

This page documents everything declared in
``components/cc1101/include/cc1101.h``. The ownership contract from
:doc:`overview` applies throughout: task context only, caller-owned
serialization, and the ``_locked`` suffix means the function requires the
caller to already hold the bus lock and never acquires one itself.

Checked functions return ``esp_err_t`` and propagate SPI/MISO failures.
The ``void`` and ``uint8_t`` compatibility wrappers only log failures;
``uint8_t`` readers return the sentinel ``0xFF`` on a failed read, never a
silent plausible zero. Use the checked variants on active paths.

Types
-----

cc1101_handle_t
~~~~~~~~~~~~~~~

.. code-block:: c

   typedef struct {
       spi_device_handle_t spi;
       gpio_num_t cs_pin;
       gpio_num_t miso_pin;
       gpio_num_t gdo0_pin;
       /* Packet polling state; caller serializes all access, including resets. */
       bool rx_length_pending;
       bool rx_recovery_pending;
       uint8_t rx_packet_length;
       int64_t rx_packet_deadline_us;
       bool tx_prepared;
   } cc1101_handle_t;

One handle per radio. ``spi`` is the caller-created SPI device handle;
``cs_pin``, ``miso_pin``, and ``gdo0_pin`` are the software chip select, the
MISO readiness input, and the packet-reception indicator. The remaining
fields are driver-owned bookkeeping: partial-frame parser state
(``rx_length_pending``, ``rx_packet_length``, ``rx_packet_deadline_us``),
the pending RX recovery flag, and the one-shot TX preparation flag. Do not
modify them from application code; the caller must serialize all access to
the handle.

cc1101_rx_format_t
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   typedef enum {
       CC1101_RX_PACKET = 0,
       CC1101_RX_RAW,
       CC1101_RX_RAW_OOK = CC1101_RX_RAW /* Source compatibility. */
   } cc1101_rx_format_t;

Packet mode uses the packet engine (sync detection, variable length, CRC).
Raw mode routes the digital demodulator output to GDO0 for any selected
modulation; it is not IQ/analog sampling and not decoded bits.
``CC1101_RX_RAW_OOK`` is an alias kept for source compatibility.

cc1101_modulation_t
~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   typedef enum {
       CC1101_PROFILE_OOK = 0,
       CC1101_PROFILE_ASK,
       CC1101_PROFILE_2FSK,
       CC1101_PROFILE_GFSK
   } cc1101_modulation_t;

The four coordinated candidate profiles accepted by
``cc1101_apply_profile_locked()``. These are frozen 26 MHz engineering
starting profiles, not arbitrary valid parameter combinations; see
:doc:`power`.

cc1101_profile_status_t
~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   typedef struct {
       uint8_t pktctrl0;
       uint8_t pktctrl1;
       uint8_t pktlen;
       uint8_t sync1;
       uint8_t sync0;
       uint8_t mcsm1;
       uint8_t mdmcfg4;
       uint8_t mdmcfg3;
       uint8_t freq2, freq1, freq0, channr, fifothr;
       uint8_t mdmcfg2;
       uint8_t deviatn;
       uint8_t agcctrl2;
       uint8_t agcctrl1;
       uint8_t agcctrl0;
       uint8_t foccfg;
       uint8_t frend0;
       uint8_t iocfg0;
       uint8_t marcstate;
       uint8_t patable[8];
   } cc1101_profile_status_t;

Snapshot of the configuration registers that define a profile, plus the
five-bit MARCSTATE and the full 8-entry PA table, filled by
``cc1101_profile_readback_locked()``.

cc1101_tx_status_t
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   typedef struct {
       uint8_t txbytes;   /* Includes bit 7: underflow. */
       uint8_t marcstate;
       bool gdo0_high;
   } cc1101_tx_status_t;

TX completion sample filled by ``cc1101_tx_status_locked()``. ``txbytes``
bit 7 flags TX FIFO underflow; ``marcstate`` is masked to five bits;
``gdo0_high`` is the sampled GDO0 level.

cc1101_rx_format_status_t
~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   typedef struct {
       uint8_t pktctrl0;
       uint8_t mdmcfg2;
       uint8_t iocfg0;
       uint8_t marcstate;
   } cc1101_rx_format_status_t;

Reduced format snapshot filled by ``cc1101_get_rx_format_status()``;
``marcstate == 0xFF`` means the readback was unavailable.

Constants
---------

Limits
~~~~~~

.. code-block:: c

   #define CC1101_FIFO_SIZE        64
   #define CC1101_MAX_PACKET_LEN   255
   #define CC1101_MAX_PAYLOAD_LEN  (CC1101_FIFO_SIZE - 3)

``CC1101_FIFO_SIZE`` is the hardware FIFO depth in bytes.
``CC1101_MAX_PAYLOAD_LEN`` (61) is the non-streaming payload bound: length
byte plus payload plus two appended status bytes must fit one FIFO.
``CC1101_MAX_PACKET_LEN`` (255) is the variable-length field maximum,
useful only as a buffer size; payloads above 61 bytes are rejected.

PA values
~~~~~~~~~

.. code-block:: c

   #define CC1101_DEFAULT_PA_433_MHZ      0xC8
   #define CC1101_CLOSE_RANGE_PA_433_MHZ  0x12
   #define CC1101_OOK_PA_433_MHZ          /* Kconfig-selected, default 0xC8 */
   #define CC1101_ASK_LOW_PA_433_MHZ      /* Kconfig-selected, default 0x34 */
   #define CC1101_ASK_HIGH_PA_433_MHZ     /* Kconfig-selected, default 0xC8 */
   #define CC1101_FSK_PA_433_MHZ          /* Kconfig-selected, default 0xC8 */

``0xC8`` is TI's nominal +7 dBm conducted setting at 433 MHz and ``0x12``
the nominal -30 dBm setting; the four derived macros resolve through the
Kconfig bench options documented in :doc:`power`.

Register map
~~~~~~~~~~~~

Configuration registers ``0x00``-``0x2E``:

.. code-block:: c

   #define CC1101_IOCFG2   0x00   #define CC1101_IOCFG1   0x01
   #define CC1101_IOCFG0   0x02   #define CC1101_FIFOTHR  0x03
   #define CC1101_SYNC1    0x04   #define CC1101_SYNC0    0x05
   #define CC1101_PKTLEN   0x06   #define CC1101_PKTCTRL1 0x07
   #define CC1101_PKTCTRL0 0x08   #define CC1101_ADDR     0x09
   #define CC1101_CHANNR   0x0A   #define CC1101_FSCTRL1  0x0B
   #define CC1101_FSCTRL0  0x0C   #define CC1101_FREQ2    0x0D
   #define CC1101_FREQ1    0x0E   #define CC1101_FREQ0    0x0F
   #define CC1101_MDMCFG4  0x10   #define CC1101_MDMCFG3  0x11
   #define CC1101_MDMCFG2  0x12   #define CC1101_MDMCFG1  0x13
   #define CC1101_MDMCFG0  0x14   #define CC1101_DEVIATN  0x15
   #define CC1101_MCSM2    0x16   #define CC1101_MCSM1    0x17
   #define CC1101_MCSM0    0x18   #define CC1101_FOCCFG   0x19
   #define CC1101_BSCFG    0x1A   #define CC1101_AGCCTRL2 0x1B
   #define CC1101_AGCCTRL1 0x1C   #define CC1101_AGCCTRL0 0x1D
   #define CC1101_WOREVT1  0x1E   #define CC1101_WOREVT0  0x1F
   #define CC1101_WORCTRL  0x20   #define CC1101_FREND1   0x21
   #define CC1101_FREND0   0x22   #define CC1101_FSCAL3   0x23
   #define CC1101_FSCAL2   0x24   #define CC1101_FSCAL1   0x25
   #define CC1101_FSCAL0   0x26   #define CC1101_RCCTRL1  0x27
   #define CC1101_RCCTRL0  0x28   #define CC1101_FSTEST   0x29
   #define CC1101_PTEST    0x2A   #define CC1101_AGCTEST  0x2B
   #define CC1101_TEST2    0x2C   #define CC1101_TEST1    0x2D
   #define CC1101_TEST0    0x2E

Command strobes:

.. code-block:: c

   #define CC1101_SRES     0x30   #define CC1101_SFSTXON  0x31
   #define CC1101_SXOFF    0x32   #define CC1101_SCAL     0x33
   #define CC1101_SRX      0x34   #define CC1101_STX      0x35
   #define CC1101_SIDLE    0x36   #define CC1101_SAFC     0x37
   #define CC1101_SWOR     0x38   #define CC1101_SPWD     0x39
   #define CC1101_SFRX     0x3A   #define CC1101_SFTX     0x3B
   #define CC1101_SWORRST  0x3C   #define CC1101_SNOP     0x3D

Status registers (read with the burst bit set):

.. code-block:: c

   #define CC1101_PARTNUM         0x30   #define CC1101_VERSION   0x31
   #define CC1101_FREQEST         0x32   #define CC1101_LQI       0x33
   #define CC1101_RSSI            0x34   #define CC1101_MARCSTATE 0x35
   #define CC1101_WORTIME1        0x36   #define CC1101_WORTIME0  0x37
   #define CC1101_PKTSTATUS       0x38   #define CC1101_VCO_VC_DAC 0x39
   #define CC1101_TXBYTES         0x3A   #define CC1101_RXBYTES   0x3B
   #define CC1101_RCCTRL1_STATUS  0x3C   #define CC1101_RCCTRL0_STATUS 0x3D
   #define CC1101_PATABLE         0x3E   #define CC1101_FIFO_ADDR 0x3F

SPI access bits:

.. code-block:: c

   #define CC1101_WRITE_BURST  0x40
   #define CC1101_READ_SINGLE  0x80
   #define CC1101_READ_BURST   0xC0

GDO output selections (for IOCFGx):

.. code-block:: c

   #define CC1101_GDO_RXFIFO_THRESHOLD   0x00
   #define CC1101_GDO_RXFIFO_OVERFLOW    0x01
   #define CC1101_GDO_TXFIFO_THRESHOLD   0x02
   #define CC1101_GDO_TXFIFO_UNDERFLOW   0x03
   #define CC1101_GDO_SYNC_WORD          0x06
   #define CC1101_GDO_CRC_OK             0x07
   #define CC1101_GDO_CARRIER_SENSE      0x0E
   #define CC1101_GDO_HIGH_Z             0x2E
   #define CC1101_GDO_HW0                0x2F

Packet control bits:

.. code-block:: c

   /* PKTCTRL0 */
   #define CC1101_PKT_FIXED       0x00
   #define CC1101_PKT_VARIABLE    0x01
   #define CC1101_PKT_INFINITE    0x02
   #define CC1101_CRC_ENABLE      (1 << 2)
   #define CC1101_DATA_WHITENING  (1 << 6)
   /* PKTCTRL1 */
   #define CC1101_APPEND_STATUS         (1 << 2)
   #define CC1101_ADR_CHK_NONE          0x00
   #define CC1101_ADR_CHK_NO_BCAST      0x01
   #define CC1101_ADR_CHK_0_BCAST       0x02
   #define CC1101_ADR_CHK_0_255_BCAST   0x03

Modulation format codes (MDMCFG2.MOD_FORMAT) and field masks:

.. code-block:: c

   #define CC1101_MOD_2FSK    0
   #define CC1101_MOD_GFSK    1
   #define CC1101_MOD_ASK_OOK 3
   #define CC1101_MOD_4FSK    4
   #define CC1101_MOD_MSK     7

   #define CC1101_MDMCFG2_MOD_FORMAT_MASK 0x70
   #define CC1101_FOCCFG_FOC_LIMIT_MASK   0x03
   #define CC1101_FREND0_PA_POWER_MASK    0x07

Sync detection modes (MDMCFG2.SYNC_MODE):

.. code-block:: c

   #define CC1101_SYNC_NONE           0
   #define CC1101_SYNC_15_16          1
   #define CC1101_SYNC_16_16          2
   #define CC1101_SYNC_30_32          3
   #define CC1101_SYNC_CARRIER_15_16  5
   #define CC1101_SYNC_CARRIER_16_16  6
   #define CC1101_SYNC_CARRIER_30_32  7

Autocalibration settings (MCSM0):

.. code-block:: c

   #define CC1101_AUTOCAL_NEVER         0x00
   #define CC1101_AUTOCAL_IDLE_TO_RXTX  0x10
   #define CC1101_AUTOCAL_RXTX_TO_IDLE  0x20
   #define CC1101_AUTOCAL_ALWAYS        0x30

Modem configuration builders:

.. code-block:: c

   #define CC1101_MDMCFG4_VALUE(chanbw_e, chanbw_m, drate_e) \
       ((((chanbw_e) & 0x3) << 6) | (((chanbw_m) & 0x3) << 4) | ((drate_e) & 0xF))
   #define CC1101_MDMCFG3_VALUE(drate_m) \
       ((drate_m) & 0xFF)
   #define CC1101_MDMCFG2_VALUE(dc_filter_off, mod_format, manchester, sync_mode) \
       ((((dc_filter_off) & 0x1) << 7) | \
        (((mod_format) & 0x7) << 4) | \
        (((manchester) & 0x1) << 3) | \
        ((sync_mode) & 0x7))
   #define CC1101_MDMCFG1_VALUE(fec_en, num_preamble, chanspc_e) \
       ((((fec_en) & 0x1) << 7) | \
        (((num_preamble) & 0x7) << 4) | \
        ((chanspc_e) & 0x3))
   #define CC1101_MDMCFG0_VALUE(chanspc_m) \
       ((chanspc_m) & 0xFF)

Frequency presets for a 26 MHz crystal (FREQ2/1/0):

.. code-block:: c

   #define CC1101_FREQ_433_92_2  0x10
   #define CC1101_FREQ_433_92_1  0xA7
   #define CC1101_FREQ_433_92_0  0x62
   #define CC1101_FREQ_868_3_2   0x21
   #define CC1101_FREQ_868_3_1   0x65
   #define CC1101_FREQ_868_3_0   0x6A
   #define CC1101_FREQ_915_0_2   0x23
   #define CC1101_FREQ_915_0_1   0x31
   #define CC1101_FREQ_915_0_0   0x3B

Initialization and reset
------------------------

cc1101_init
~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_init(cc1101_handle_t *dev,
                         spi_device_handle_t spi_handle,
                         gpio_num_t cs,
                         gpio_num_t miso,
                         gpio_num_t gdo0);

Bind the handle to an existing SPI device and pins, run the full hardware
reset sequence, and verify chip identity. Succeeds only when PARTNUM reads
0 and VERSION is neither 0 nor 0xFF.

* ``dev``: handle to initialize; fully overwritten on entry.
* ``spi_handle``: caller-created SPI device (mode 0, software CS).
* ``cs``, ``miso``, ``gdo0``: GPIO numbers; negative values are rejected.
* Returns ``ESP_OK`` on success, ``ESP_ERR_INVALID_ARG`` for a NULL handle,
  NULL SPI handle, or negative pin, ``ESP_ERR_TIMEOUT`` if MISO readiness
  or the post-reset IDLE confirmation times out, ``ESP_FAIL`` if the chip
  identity check fails, or the underlying SPI error.

cc1101_reset
~~~~~~~~~~~~

.. code-block:: c

   void cc1101_reset(cc1101_handle_t *dev);

Compatibility wrapper around the checked reset: CS pulse train, SRES
strobe with MISO readiness waits, datasheet delay, and confirmed IDLE.
Clears all parser and TX preparation state. Logs failures instead of
returning them.

Configuration
-------------

cc1101_config_checked
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_config_checked(cc1101_handle_t *dev);

Write the deterministic baseline OOK packet profile after reset (see
:doc:`overview` for the parameters), disable the FOC limit, zero then load
the PA table via ``cc1101_set_tx_power()``, and finish with a manual SCAL
calibration confirmed in IDLE. Never starts TX or RX.

* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG`` for a NULL handle, or the
  first transport/state error encountered.

cc1101_config
~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_config(cc1101_handle_t *dev);

Compatibility wrapper around ``cc1101_config_checked()``; logs failures.

cc1101_set_frequency
~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_frequency(cc1101_handle_t *dev, uint32_t freq_hz);

Program the 24-bit carrier frequency word ``FREQ = freq_hz * 2^16 /
26 MHz`` into FREQ2/1/0. The written word is the quantized frequency, not
necessarily the exact request.

* ``freq_hz``: requested carrier in Hz.
* Returns ``ESP_OK`` or the first register-write error.

cc1101_set_channel
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_channel(cc1101_handle_t *dev, uint8_t channel);

Write the CHANNR register. The channel spacing comes from MDMCFG1/0 as
previously configured; this function only selects the channel number.

* Returns ``ESP_OK`` or the register-write error.

cc1101_set_datarate
~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_datarate(cc1101_handle_t *dev, uint32_t baud);

Quantize a requested symbol rate into DRATE_E/DRATE_M for the 26 MHz
crystal, preserving the RX bandwidth bits of MDMCFG4. Picks the first
exponent whose mantissa lands in [256, 511]; if none fits, exponent and
mantissa stay 0. The written value is the quantized rate (38400 baud
becomes approximately 38.383 kbps).

* ``baud``: requested rate in baud.
* Returns ``ESP_OK`` or the first read/write error.

cc1101_set_tx_power
~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_tx_power(cc1101_handle_t *dev, uint8_t pa_value);

Set the PA output level while preserving the current modulation's PA
layout. For ASK/OOK it reads MDMCFG2 and the existing PA table: true ASK
keeps its configured low level (``CC1101_ASK_LOW_PA_433_MHZ``), otherwise
entry 0 is off (0x00); the high level becomes ``pa_value`` with
FREND0.PA_POWER = 1. Other modulations write ``pa_value`` to entry/index 0.

* ``pa_value``: raw PA table byte, e.g. ``CC1101_DEFAULT_PA_433_MHZ``.
* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG`` for a NULL handle, or the
  first transport error. Caller owns bus serialization.

cc1101_set_tx_len
~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_tx_len(cc1101_handle_t *dev, uint8_t len);

Write the PKTLEN maximum packet length register.

* ``len``: maximum payload length; values above ``CC1101_MAX_PAYLOAD_LEN``
  (61) are rejected because non-streaming frames must fit the FIFO.
* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG``, or the write error.

Profiles and state control
--------------------------

cc1101_idle_locked
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_idle_locked(cc1101_handle_t *dev);

Strobe SIDLE and poll MARCSTATE until IDLE (0x01) is confirmed, with a
~20 ms bound. Clears ``tx_prepared``.

* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG``, ``ESP_ERR_TIMEOUT``, or the
  transport error.

cc1101_apply_profile_locked
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_apply_profile_locked(cc1101_handle_t *dev,
                                        cc1101_modulation_t modulation,
                                        cc1101_rx_format_t format);

Apply one coordinated modulation/format profile, verified end to end.
Validates the enums before any I/O, confirms IDLE before flushing both
FIFOs, writes the deterministic register delta, verifies every written
register plus the PA table through a full readback, and leaves the chip in
IDLE. It never starts TX or RX. The caller also owns GDO interrupt/capture
suspension and application state validity. Partial failure is not rolled
back: the caller must invalidate its applied-state bookkeeping.

* ``modulation``: one of the four ``cc1101_modulation_t`` values.
* ``format``: ``CC1101_RX_PACKET`` or ``CC1101_RX_RAW``.
* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG`` for invalid enums or NULL
  handle, ``ESP_ERR_TIMEOUT`` from state confirmation, ``ESP_FAIL`` on a
  readback mismatch, or the first transport error.

cc1101_start_rx_locked
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_start_rx_locked(cc1101_handle_t *dev);

Strobe SRX and confirm the RX state (MARCSTATE 0x0D). On failure a
best-effort IDLE is attempted; the original error is returned.

* Returns ``ESP_OK``, ``ESP_ERR_TIMEOUT``, or the transport error.

cc1101_set_rx_format
~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_set_rx_format(cc1101_handle_t *dev, cc1101_rx_format_t format);

Switch the RX format while preserving the modulation read back from
hardware (MDMCFG2 format bits plus, for ASK/OOK, the PA table pattern).
Runs IDLE, a full readback, a profile apply, then ``cc1101_start_rx_locked()``.
The caller must own the bus lock and disable the associated GDO interrupt.
Failure does not arm RX; nothing is flushed unless IDLE was confirmed
first.

* ``format``: ``CC1101_RX_PACKET`` or ``CC1101_RX_RAW``.
* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG``,
  ``ESP_ERR_INVALID_STATE`` if the hardware modulation/PA pattern is not
  recognized, or the first transport/readback error.

cc1101_set_tx_mode
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_set_tx_mode(cc1101_handle_t *dev);

Compatibility helper: confirmed IDLE plus a TX FIFO flush (SFTX). Logs
failures.

cc1101_set_rx_mode
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_set_rx_mode(cc1101_handle_t *dev);

Compatibility RX restart: parser reset, IDLE, SFRX, then
``cc1101_start_rx_locked()``. Marks RX recovery pending on failure so the
next packet health check retries it. Logs failures.

Transmit
--------

The checked TX path is prepare, start exactly once, then poll status.
There are no automatic retries.

cc1101_tx_prepare_locked
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_tx_prepare_locked(cc1101_handle_t *dev, const uint8_t *data, size_t len);

Load one length-prefixed packet into the TX FIFO without transmitting.
Verifies IDLE, flushes the TX FIFO, confirms empty occupancy with a stable
status read, checks the packet framing registers (variable length + CRC,
GDO0 sync-word output, ``len <= PKTLEN``), writes length + payload in one
contiguous burst, and re-verifies exact FIFO occupancy. Sets
``tx_prepared`` for ``cc1101_tx_start_locked()``.

* ``data``: payload bytes; NULL rejected.
* ``len``: 1 to ``CC1101_MAX_PAYLOAD_LEN`` (61).
* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG`` for bad arguments,
  ``ESP_ERR_INVALID_STATE`` for incompatible framing registers,
  ``ESP_FAIL`` if FIFO occupancy does not match, or the transport error.

cc1101_tx_start_locked
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_tx_start_locked(cc1101_handle_t *dev);

Issue the single STX strobe for a prepared packet, exactly once.
Preparation is consumed even on error: the strobe may have reached the
chip before a transport failure, so a potentially started TX is never
retried by this driver.

* Returns ``ESP_OK``, ``ESP_ERR_INVALID_STATE`` if no packet is prepared
  or the handle is NULL, or the transport error.

cc1101_tx_status_locked
~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_tx_status_locked(cc1101_handle_t *dev, cc1101_tx_status_t *out);

Sample TXBYTES and MARCSTATE with stable (double-agreeing) reads plus the
GDO0 level into ``*out``. TXBYTES bit 7 flags FIFO underflow; MARCSTATE is
masked to five bits. Published only after all reads succeed.

* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG``, ``ESP_ERR_TIMEOUT`` from
  unstable status, or the transport error.

cc1101_transmit
~~~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_transmit(cc1101_handle_t *dev, uint8_t *data, size_t len);

Compatibility one-shot transmit: ``cc1101_tx_prepare_locked()`` followed by
``cc1101_tx_start_locked()``. Logs failures; no retry.

Receive
-------

The poll functions require the framing this driver configures: variable
length, APPEND_STATUS, hardware CRC, no CRC autoflush, and
MCSM1.RXOFF_MODE = RX. After a successful read the receiver stays in RX
with no SIDLE/SFRX, so queued packets survive. Partial frames are retained
between calls; malformed, stalled, or overflowed reception is discarded
via confirmed IDLE -> SFRX -> RX recovery. Caller owns serialization.

cc1101_poll_packet_with_status
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   bool cc1101_poll_packet_with_status(cc1101_handle_t *dev,
                                       uint8_t *buffer,
                                       size_t *len,
                                       bool *crc_ok,
                                       uint8_t *rssi_status,
                                       uint8_t *lqi_crc_status);

Consume one complete packet if present, reporting the hardware FCS
outcome. A frame that failed CRC is still reported (``true`` with
``*crc_ok = false``). Only complete frames are consumed; other queued
frames remain in the FIFO.

* ``buffer``: destination for the payload.
* ``len``: in, the buffer capacity (pass it on every call); out, the
  received payload length. Lengths above 61 or above the capacity trigger
  FIFO recovery.
* ``crc_ok``: out, hardware CRC outcome (bit 7 of the second status byte).
* ``rssi_status``, ``lqi_crc_status``: optional outputs for the two raw
  appended status bytes; may be NULL.
* Returns ``true`` when a complete frame was consumed, ``false`` otherwise
  (nothing ready, incomplete frame retained, or recovery performed).

cc1101_poll_packet
~~~~~~~~~~~~~~~~~~

.. code-block:: c

   bool cc1101_poll_packet(cc1101_handle_t *dev,
                           uint8_t *buffer,
                           size_t *len,
                           bool *crc_ok);

``cc1101_poll_packet_with_status()`` without the optional RSSI/LQI outputs.

cc1101_receive_packet
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   bool cc1101_receive_packet(cc1101_handle_t *dev,
                              uint8_t *buffer,
                              size_t *len);

Legacy receive: returns ``true`` only for a complete frame with a good
CRC. A CRC-failed frame is consumed but reported as ``false``,
indistinguishable from "nothing received"; prefer ``cc1101_poll_packet()``
when the FCS outcome matters.

cc1101_packet_rx_ready_locked
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_packet_rx_ready_locked(cc1101_handle_t *dev);

Packet-mode health gate for a manual TX on another radio: checks RX
health/recovery without consuming pending data, then requires the radio
to be in RX with an empty FIFO, no partial frame, and GDO0 low. Any
queued/partial frame, non-RX state, or asserted GDO0 rejects the request.

* Returns ``ESP_OK`` when idle-listening, ``ESP_ERR_INVALID_ARG``,
  ``ESP_ERR_INVALID_STATE`` when not ready, or the health-check error.

Status readback
---------------

cc1101_profile_readback_locked
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   esp_err_t cc1101_profile_readback_locked(cc1101_handle_t *dev,
                                          cc1101_profile_status_t *status);

Read back the full profile image from hardware: 21 configuration
registers, MARCSTATE, and the 8-entry PA table. Publishes ``*status`` only
after every checked read succeeds; an all-zero bus image or an undefined
MARCSTATE (above 0x16) fails even if each SPI call succeeded.

* Returns ``ESP_OK``, ``ESP_ERR_INVALID_ARG``, ``ESP_FAIL`` for the
  implausible-image checks, or the first transport error.

cc1101_get_rx_format_status
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_get_rx_format_status(cc1101_handle_t *dev, cc1101_rx_format_status_t *status);

Legacy void snapshot of PKTCTRL0, MDMCFG2, IOCFG0, and MARCSTATE via a full
readback. On any failure the struct is zeroed with ``marcstate = 0xFF`` as
the unavailable marker.

cc1101_read_status_reg
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   uint8_t cc1101_read_status_reg(cc1101_handle_t *dev, uint8_t reg);

Read one status register (burst access). Compatibility semantics: logs
failures and returns the ``0xFF`` sentinel. For dynamic multi-bit status
on active paths the driver internally uses stable double-agreeing reads
instead.

cc1101_dump_registers
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

   void cc1101_dump_registers(cc1101_handle_t *dev);

Debug aid: log every configuration register (0x00-0x2E) plus PARTNUM,
VERSION, and MARCSTATE at info level. Failed reads log an error and show
the ``0xFF`` sentinel.

Strobes
-------

cc1101_strobe
~~~~~~~~~~~~~

.. code-block:: c

   uint8_t cc1101_strobe(cc1101_handle_t *dev, uint8_t strobe);

Issue one command strobe (``CC1101_SRES``, ``CC1101_SRX``, ``CC1101_STX``,
etc.) with a MISO readiness wait and return the chip status byte clocked
out during the strobe. Logs transport failures and returns ``0xFF``
instead of a plausible status; NULL handle or SPI handle also yields
``0xFF``.
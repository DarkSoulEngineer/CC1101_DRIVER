Overview
========

``components/cc1101`` is a reusable, handle-based ESP-IDF driver for the
Texas Instruments CC1101 sub-1 GHz transceiver. It provides a checked SPI
transport, deterministic register profiles, explicit manual TX, and packet
RX with errata-aware FIFO recovery. It contains no application policy, no
board pin assignments, and no background task: nothing transmits unless the
application explicitly starts TX.

The manual is split into four pages:

* :doc:`api`: complete public API reference for ``cc1101.h``.
* :doc:`internals`: how the driver works inside, with annotated source
  snippets.
* :doc:`power`: PA tables, power levels, Kconfig bench options, and
  register quantization.

The full driver files are also rendered verbatim in :doc:`source`.

Integration
-----------

1. Copy ``components/cc1101`` into your project's ``components/`` directory.
2. Add ``cc1101`` to the consuming component's ``REQUIRES`` (or
   ``PRIV_REQUIRES`` if ``cc1101.h`` is not exposed in your public headers).
   The component itself depends only on ESP-IDF's SPI, GPIO, FreeRTOS,
   esp_rom, esp_timer, and log components.
3. Initialize the SPI bus and GPIO yourself before calling ``cc1101_init()``:

   * SPI mode 0; 1 MHz is a reasonable starting clock.
   * Software chip select: set ``.spics_io_num = -1`` in the device
     configuration and preload the CS GPIO high before enabling it. The
     driver asserts and releases CS around every transaction.
   * The bus must support transactions of at least 65 bytes (one command
     byte plus up to 64 data bytes). Use ``SPI_DMA_CH_AUTO`` on ESP32-S3.
   * MISO must remain readable as a GPIO input after SPI setup: the driver
     polls it to wait for chip readiness before clocking.
   * GDO0 is a GPIO input used by the packet RX path to track reception in
     progress. GDO2 is not required by the driver.

4. Give each radio its own ``cc1101_handle_t`` with its own SPI device
   handle and pins, so several modules can run on separate buses.

Use ESP-IDF v6.1; the reference MCU is the ESP32-S3. Other targets and
ESP-IDF versions are not claimed to be validated.

Ownership contract
------------------

The driver takes no mutex. Call it from task context, never from an ISR.
The caller must serialize all access to a handle and its radio, including
resets, FIFO service, and profile changes; if the SPI bus is shared, other
traffic must not interleave with a software-CS transaction. The ``_locked``
suffix marks functions that require the caller to already own that
serialization; none of them acquire a lock themselves.

Minimal usage
-------------

.. code-block:: c

   cc1101_handle_t radio;
   cc1101_init(&radio, spi_device, PIN_CS, PIN_MISO, PIN_GDO0);
   cc1101_config(&radio);                     /* 433.92 MHz OOK packet baseline */
   cc1101_set_frequency(&radio, 433920000);   /* Hz */
   cc1101_set_datarate(&radio, 38400);        /* baud */
   cc1101_set_rx_mode(&radio);

   uint8_t buf[CC1101_MAX_PACKET_LEN];
   size_t len = sizeof(buf);
   if (cc1101_receive_packet(&radio, buf, &len)) {
       /* len bytes received, CRC passed */
   }

The ``void`` functions in this example are compatibility wrappers that log
failures instead of returning them. Active code should prefer the checked
variants (``cc1101_config_checked()``, ``cc1101_start_rx_locked()``,
``cc1101_poll_packet()``) so transport and state errors reach the
application; see :doc:`api`.

Baseline configuration
----------------------

``cc1101_config_checked()`` writes a deterministic OOK packet profile:
433.92 MHz, 38400 baud requested (approximately 38.383 kbps after register
quantization), 101.5625 kHz RX bandwidth, sync word ``D391`` with 30/32
detection, variable-length packets with hardware CRC and appended status,
and a maximum payload of ``CC1101_MAX_PAYLOAD_LEN`` (61) bytes.
``cc1101_apply_profile_locked()`` extends this to coordinated OOK, ASK,
2-FSK, and GFSK profiles in packet or raw digital RX format; the power
levels and their caveats are covered in :doc:`power`.

References
----------

* `CC1101 datasheet (SWRS061I) <https://www.ti.com/lit/ds/symlink/cc1101.pdf>`_
* `CC1101 silicon errata (SWRZ020) <https://www.ti.com/lit/swrz020>`_
* `TI DN013: 433 MHz PA tables <https://www.ti.com/lit/an/swra151a/swra151a.pdf>`_
* `Driver repository <https://github.com/DarkSoulEngineer/CC1101_DRIVER>`_
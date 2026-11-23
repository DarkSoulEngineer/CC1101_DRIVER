CC1101 driver internals
=======================

This page walks through ``components/cc1101/src/cc1101.c`` with verbatim
snippets. The public contracts are in :doc:`api`; the ownership
rules are in :doc:`overview`.

SPI transport and MISO readiness
--------------------------------

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 50-58
   :lineno-match:

The CC1101 holds MISO high until it is ready to accept a transaction, so
the driver also wires MISO as a plain GPIO input and polls it after
asserting CS. The wait is bounded to about 2 ms (2000 iterations of a 1 us
delay); a stuck or absent chip fails with ``ESP_ERR_TIMEOUT`` instead of
hanging the caller.

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 76-102
   :lineno-match:

This is the body of ``cc1101_profile_transfer()``, the single transport
primitive behind every register access and FIFO burst. The command byte
and up to 64 payload bytes travel in one SPI transaction under one CS
assertion, so a length-prefixed packet is never split across transfers.
Transfers that fit the SPI driver's four inline data bytes avoid external
buffer setup; longer ones use bounded 65-byte stack buffers. On reads, the
first received byte (the chip status shifted out during the command) is
skipped and only the payload is copied out. Any MISO or SPI error is
returned to the caller; the checked paths never manufacture a plausible
zero from a failed read.

Checked configuration and readback verification
-----------------------------------------------

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 283-306
   :lineno-match:

``cc1101_profile_readback_locked()`` samples the 21 configuration
registers that define a profile (via the ``s_readback_regs`` table and a
matching ``offsetof`` table, so the two cannot drift apart), then
MARCSTATE and the full 8-entry PA table. Every read is checked, and the
result is published to ``*status`` only after all of them succeed. Two
plausibility gates reject a bus that answers but is not really there: an
all-zero image, and a MARCSTATE above 0x16, both return ``ESP_FAIL`` even
though each individual SPI call succeeded.

Profile apply
-------------

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 345-376
   :lineno-match:

This is the heart of ``cc1101_apply_profile_locked()``. It selects the
modem format bits (0x30 for amplitude OOK/ASK, 0x10 for GFSK, 0x00 for
2-FSK), builds the PA table from the Kconfig-selected levels, and writes a
deterministic register delta: 433.92 MHz, MDMCFG4/3 = CA/83
(~38.383 kbps), DEVIATN = 0x35, sync word D391, 61-byte payload limit,
appended status, and format-dependent PKTCTRL0/MDMCFG2/IOCFG0 values.
FREND0 and MDMCFG1 are read-modify-write so unrelated fields survive.
Before this block, the function has validated the enums and confirmed
IDLE before flushing both FIFOs; after it, every written register is read
back and compared byte for byte, the PA table and IDLE state are verified
through a full readback, and any mismatch returns ``ESP_FAIL``. The chip
is left in IDLE and partial failure is not rolled back.

TX FIFO fill, start, and underflow check
----------------------------------------

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 544-570
   :lineno-match:

``cc1101_tx_prepare_locked()`` loads the FIFO but never transmits. It
confirms IDLE, flushes the TX FIFO, and requires a stable TXBYTES read of
zero before writing anything. The framing registers must match the
driver's packet mode (variable length + CRC, sync-word GDO0) and the
payload must fit PKTLEN. Length byte plus payload then go out as one
contiguous burst, and the exact occupancy ``len + 1`` is re-verified with
another stable read; only then is ``tx_prepared`` set.
``cc1101_tx_start_locked()`` consumes that flag and issues STX exactly
once, even if the strobe transfer reports an error, because the command
may already have reached the chip. Completion is judged afterwards with
``cc1101_tx_status_locked()``, where TXBYTES bit 7 flags FIFO underflow.

Packet RX and errata handling
-----------------------------

The RX path follows TI's silicon errata SWRZ020E in two places. Dynamic
multi-bit status registers (RXBYTES, TXBYTES, MARCSTATE) can be caught
mid-update, so ``cc1101_read_stable_status()`` reads until two consecutive
reads agree, with a bounded retry count; persistent disagreement returns
``ESP_ERR_TIMEOUT`` and is never treated as permission to consume or flush
the FIFO. The RX FIFO pointer erratum means the FIFO must not be drained
before a frame is complete, which is why the parser retains partial
frames across calls.

.. literalinclude:: ../components/cc1101/src/cc1101.c
   :language: c
   :lines: 824-847
   :lineno-match:

Inside ``cc1101_poll_packet_with_status()``, once a length byte has been
latched the driver waits until ``length + 2`` bytes are present: payload
plus the two appended RSSI/LQI status bytes. An invalid or oversized
length discards the FIFO through confirmed IDLE -> SFRX -> RX recovery
instead of reading garbage. On success the CRC outcome comes from bit 7
of the second status byte, the parser state is reset, and the receiver
stays in RX with no SIDLE/SFRX, so other queued packets survive and no
calibration deaf interval is created. Before any of this,
``cc1101_packet_rx_health()`` checks for overflow (RXBYTES bit 7, a count
beyond the physical FIFO, or the RXFIFO_OVERFLOW MARCSTATE 0x11, which can
stick without the flag per SWRZ020E) and an incomplete-frame deadline
derived from the actual MDMCFG4/3 rate registers triggers the same
recovery for stalled reception.
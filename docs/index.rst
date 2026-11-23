CC1101 Driver documentation
===========================

A standalone ESP-IDF component for the Texas Instruments CC1101 sub-1 GHz
transceiver: a handle-based SPI interface, checked radio configuration,
explicit manual TX, and packet RX with errata-aware FIFO recovery. No
application, board pin assignments, or background task is included;
nothing transmits unless the application explicitly starts TX.

.. toctree::
   :maxdepth: 2
   :caption: Driver manual

   overview
   api
   internals
   power

.. toctree::
   :maxdepth: 1
   :caption: Source and README

   source
   readme

* :ref:`genindex`
* :ref:`search`
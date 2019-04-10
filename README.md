Streaming data management tool

INSTALLATION:
    make
    sudo make install

USAGE:
    // C Source file
    #include <bingewatch/[tool].h>

    # Makefile
    -lbingewatch

API:
    <bingewatch/machine.h>
    A machine is a blueprint for instantiating a bidirectional input/output node.

    <bingewatch/filter.h>
    A filter attaches to the input or output of a machine, and accesses or
    mutates data.  Filters can be chained together.  The API is very strict, and
    allows for extensible functionality.

    <bingewatch/stream.h>
    A stream is a network of machines passing data asynchronously.

IMPLEMENTATIONS:
    <bingewatch/block-list-buffer.h>
    Generic data buffer machine.  Each instantiation is essintially a linked
    list of asynchronous buffers.

    <bingewatch/simple-machines.h>
        - File Machine
        - Socket Machine

    <bingewatch/simple-buffers.h>
        - Flexible Ring Buffer: ring buffer with auto resizing

    <bingewatch/simple-filters.h>
        - Byte Count Limiter: Returns "complete" when threshold is reached
        - Dump to Binfile: Taps data and writes bytes to a binary file
        - Byte Counter: Counts bytes and prints count periodically
        - Conversion Filter: Converts data between common types

    <bingewatch/sdr-machine.h>
    Generic machine for interfacing with Software Defined Radios.

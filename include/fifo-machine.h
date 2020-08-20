#ifndef __FIFO_MACHINE_H__
#define __FIFO_MACHINE_H__

// FLAGS
#define MFIFO_MODE  0x00000003
#define FFIFO_READ  0x00000001
#define FFIFO_WRITE 0x00000002
#define FFIFO_RW    FFIFO_READ | FFIFO_WRITE

#define FFIFO_LEAVE_OPEN 0x00000010

// ARG STRUCT
struct fifoiom_args {
    // Read/Write
    char *fname;
    uint32_t flags;
};

extern const IOM *fifo_machine;

const IOM *get_fifo_machine();

IO_HANDLE new_fifo_machine(char *fname, uint32_t flags);

#endif

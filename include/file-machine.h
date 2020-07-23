#ifndef __FILE_MACHINE_H__
#define __FILE_MACHINE_H__

// FLAGS
#define MFILE_MODE  0x00000003
#define FFILE_READ  0x00000001
#define FFILE_WRITE 0x00000002
#define FFILE_RW    FFILE_READ | FFILE_WRITE

#define FFILE_ROTATE      0x00000010
#define FFILE_AUTO_ROTATE 0x00000020
#define FFILE_DIR_ROTATE  0x00000040

#define FFILE_AUTO_DATE   0x00000100

// ARG STRUCT
struct fileiom_args {
    // Read/Write
    char *fname;
    uint32_t flags;

    // Write
    char *root_dir;
    char *base_dir;
    char *ext;
};

const IOM *get_file_machine();

IO_HANDLE new_file_machine(char *rootdir, char *fname, char *ext, uint32_t flags);
void file_iom_set_flags(IO_HANDLE h, uint32_t flags);

#endif

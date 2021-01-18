#ifndef __BINGEWATCH_LOGGING_H__
#define __BINGEWATCH_LOGGING_H__

void segment_set_log_level(char *level);
void stream_set_log_level(char *level);
void machine_set_log_level(char *level);
void machine_mgmt_set_log_level(char *level);
void rb_set_log_level(char *level);
void fbb_set_log_level(char *level);
void blb_set_log_level(char *level);

// SDRS
void sdrrx_set_log_level(char *level);

void lime_set_log_level(char *level);
void soapy_set_log_level(char *level);

void b210_set_log_level(char *level);
void uhd_set_log_level(char *level);

#endif

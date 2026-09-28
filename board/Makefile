export PARAM_FILE := ../../../../Makefile.param
include $(PARAM_FILE)

COMMON_DIR := /home/hero/Workspace/SS928V100_SDK_V2.0.2.2/smp/a55_linux/mpp/sample/common

TARGET := heshi_v2_dual_infer
SRCS := heshi_v2_dual_infer.c alert_dispatch.c alert_config.c status_report.c sht3x.c
OBJS := $(SRCS:%.c=%.o)

CC ?= $(CROSS_COMPILE)gcc

CFLAGS += -Wall -O2 -g
CFLAGS += -I$(REL_INC)
CFLAGS += -I$(REL_INC)/npu
CFLAGS += -I$(COMMON_DIR)
CFLAGS += -D$(OT_ARCH) -D$(OT_FPGA) -DISP_V2
CFLAGS += -DSENSOR0_TYPE=OV_OS08A20_MIPI_8M_30FPS_12BIT
CFLAGS += -DSENSOR1_TYPE=OV_OS08A20_MIPI_8M_30FPS_12BIT
CFLAGS += $(LIBS_CFLAGS)
CFLAGS += $(LIBS_EXEC_CFLAGS)
CFLAGS += $(MPP_CFLAGS)

LDFLAGS += $(LIBS_LD_CFLAGS)

AUDIO_LIBA := $(REL_LIB)/libss_voice_engine.a
AUDIO_LIBA += $(REL_LIB)/libss_upvqe.a
AUDIO_LIBA += $(REL_LIB)/libss_dnvqe.a
AUDIO_LIBA += $(REL_LIB)/libaac_comm.a
AUDIO_LIBA += $(REL_LIB)/libaac_enc.a
AUDIO_LIBA += $(REL_LIB)/libaac_dec.a
AUDIO_LIBA += $(REL_LIB)/libaac_sbr_enc.a
AUDIO_LIBA += $(REL_LIB)/libaac_sbr_dec.a
AUDIO_LIBA += $(REL_LIB)/libvqe_res.a
AUDIO_LIBA += $(REL_LIB)/libvqe_record.a
AUDIO_LIBA += $(REL_LIB)/libvqe_hpf.a
AUDIO_LIBA += $(REL_LIB)/libvqe_anr.a
AUDIO_LIBA += $(REL_LIB)/libvqe_aec.a
AUDIO_LIBA += $(REL_LIB)/libvqe_agc.a
AUDIO_LIBA += $(REL_LIB)/libvqe_eq.a
AUDIO_LIBA += $(REL_LIB)/libvqe_talkv2.a
AUDIO_LIBA += $(REL_LIB)/libvqe_wnr.a

MPI_LIBS := $(REL_LIB)/libss_mpi.a
MPI_LIBS += $(REL_LIB)/libss_hdmi.a
MPI_LIBS += $(REL_LIB)/libss_ae.a
MPI_LIBS += $(REL_LIB)/libss_isp.a
MPI_LIBS += $(REL_LIB)/libot_isp.a
MPI_LIBS += $(REL_LIB)/libss_awb.a
MPI_LIBS += $(REL_LIB)/libss_dehaze.a
MPI_LIBS += $(REL_LIB)/libss_extend_stats.a
MPI_LIBS += $(REL_LIB)/libss_drc.a
MPI_LIBS += $(REL_LIB)/libss_ldci.a
MPI_LIBS += $(REL_LIB)/libss_crb.a
MPI_LIBS += $(REL_LIB)/libss_bnr.a
MPI_LIBS += $(REL_LIB)/libss_calcflicker.a
MPI_LIBS += $(REL_LIB)/libss_ir_auto.a
MPI_LIBS += $(REL_LIB)/libss_acs.a
MPI_LIBS += $(REL_LIB)/libsns_os08a20.a
MPI_LIBS += $(REL_LIB)/libsns_os05a10_2l_slave.a
MPI_LIBS += $(REL_LIB)/libsns_imx347_slave.a
MPI_LIBS += $(REL_LIB)/libsns_imx485.a
MPI_LIBS += $(REL_LIB)/libsns_os04a10.a
MPI_LIBS += $(REL_LIB)/libsns_os08b10.a

COMM_OBJ := \
	$(COMMON_DIR)/sample_comm_sys.o \
	$(COMMON_DIR)/sample_comm_vi.o \
	$(COMMON_DIR)/sample_comm_vpss.o \
	$(COMMON_DIR)/sample_comm_vo.o \
	$(COMMON_DIR)/sample_comm_isp.o

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(COMM_OBJ) $(OBJS)
	$(CC) $(CFLAGS) -lpthread -lm -o $@ $^ -Wl,--start-group $(MPI_LIBS) $(AUDIO_LIBA) $(REL_LIB)/libsecurec.a -Wl,--end-group -L$(REL_LIB)/npu/stub -lascendcl -ldl -lstdc++ $(LDFLAGS)

test: test_cam_only

test_cam_only: $(COMM_OBJ) test_cam_only.o
	$(CC) $(CFLAGS) -lpthread -lm -o $@ $^ -Wl,--start-group $(MPI_LIBS) $(AUDIO_LIBA) $(LDFLAGS) $(REL_LIB)/libsecurec.a -Wl,--end-group

clean:
	@rm -f $(TARGET) $(OBJS)

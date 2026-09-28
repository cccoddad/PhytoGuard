/*
 * PlantGuard 板端主程序。
 *
 * 整体职责：
 * 1. 负责摄像头媒体管线的启动、接管和释放。
 * 2. 从 VPSS 抓取当前画面，保存为 PPM，并转换为 RGB 参与推理。
 * 3. 完成与训练流程一致的图像预处理，再调用 ACL 在 NPU 上执行模型。
 * 4. 支持单阶段 16 类分类，以及“作物识别 -> 病害识别”的两阶段模式。
 * 5. 把结果写入 JSON，并在满足条件时联动告警与设备状态上报模块。
 *
 * 运行模式：
 * - 默认模式：程序内部启动单摄像头 VI/VPSS/VO 链路。
 * - --attach-only：只接入已运行的 VPSS，适合调试。
 * - --reload-media：先重置底层媒体栈，再进入推理流程。
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#include "acl.h"
#include "sample_comm.h"
#include "securec.h"
#include "alert_dispatch.h"
#include "alert_config.h"
#include "status_report.h"
#include "sht3x.h"

/* ACL 设备号。当前程序默认只跑在 0 号 NPU 设备上。 */
#define HESHI_DEVICE_ID 0
/* 结果里保留前 5 个最高分分类，便于调试模型输出。 */
#define HESHI_TOPK 5
/* 最多支持读取多少个标签。 */
#define HESHI_MAX_LABELS 128
/* 单个标签名最大长度，给组合标签 "Plant___Disease" 也预留足够空间。 */
#define HESHI_LABEL_LEN 160

/* 单阶段模型和输出文件的默认路径。 */
#define DEFAULT_MODEL_PATH "/mnt/heshi_v2/heshi_16cls_v2.om"
#define DEFAULT_LABELS_PATH "/mnt/heshi_v2/labels.txt"
#define DEFAULT_JSON_PATH "/mnt/heshi_v2/last_result.json"
#define DEFAULT_PPM_PATH "/mnt/heshi_v2/camera_frame.ppm"
#define DEFAULT_DEVICE_ID "ss928-001"
/* 媒体栈恢复脚本所在目录与日志输出位置。 */
#define DEFAULT_LOAD_DIR "/opt/ko"
#define DEFAULT_LOAD_SCRIPT "./load_ss928v100"
#define DEFAULT_LOAD_LOG "/tmp/heshi_v2_load.log"

/* 摄像头 / VPSS 默认编号。
 * 当前项目实际只使用单摄像头，所以 camera、grp、chn 默认都是 0。
 */
#define DEFAULT_CAMERA_ID 0
#define DEFAULT_VPSS_GRP 0
#define DEFAULT_VPSS_CHN 0
/* 程序虽然保留了“数组化”的字段写法，但本项目当前只跑 1 路摄像头。 */
#define HESHI_CAMERA_NUM 1
/* 系统视频缓冲池数量配置。 */
#define HESHI_VB_WDR_RAW_CNT 8
#define HESHI_VB_DOUBLE_YUV_CNT 15
/* 推理主循环默认每隔 2 秒跑一轮。 */
#define DEFAULT_INTERVAL_SEC 2
/* 连续命中 3 次才真正触发告警，降低偶发误检的影响。 */
#define DEFAULT_CONSECUTIVE_N 3  /* need 3 consecutive same detections before alert */
/* 告警概率阈值，低于它即使命中了病害类也不累计告警次数。 */
#define DEFAULT_ALERT_THRESHOLD 0.10f
/* 1.0 表示按默认策略走“短边正方形中心裁剪”。 */
#define DEFAULT_CENTER_CROP_RATIO 1.0f
#define DEFAULT_ALERT_CONFIG_PATH "/mnt/heshi_v2/alert_config.json"

/* 两阶段推理用到的模型和标签路径定义。 */
#define HESHI_TWO_STAGE_DIR "/mnt/heshi_v2/two_stage"
#define HESHI_PLANT_MODEL     HESHI_TWO_STAGE_DIR "/plant.om"
#define HESHI_PLANT_LABELS    HESHI_TWO_STAGE_DIR "/plant_labels.txt"
#define HESHI_STAGE2_MODELS 5

typedef struct {
    /* plant_name 是 Stage1 植物分类的输出前缀，
     * 后面两个字段给出该植物对应的病害模型和标签文件路径。
     */
    const char *plant_name;
    const char *model_path;
    const char *labels_path;
} stage2_model_def_t;

static const stage2_model_def_t g_stage2_defs[HESHI_STAGE2_MODELS] = {
    {"Apple",  HESHI_TWO_STAGE_DIR "/disease_apple.om",  HESHI_TWO_STAGE_DIR "/disease_apple_labels.txt"},
    {"Corn",   HESHI_TWO_STAGE_DIR "/disease_corn.om",   HESHI_TWO_STAGE_DIR "/disease_corn_labels.txt"},
    {"Grape",  HESHI_TWO_STAGE_DIR "/disease_grape.om",  HESHI_TWO_STAGE_DIR "/disease_grape_labels.txt"},
    {"Potato", HESHI_TWO_STAGE_DIR "/disease_potato.om", HESHI_TWO_STAGE_DIR "/disease_potato_labels.txt"},
    {"Tomato", HESHI_TWO_STAGE_DIR "/disease_tomato.om", HESHI_TWO_STAGE_DIR "/disease_tomato_labels.txt"},
};

typedef struct {
    /* model_id: ACL 加载模型后返回的句柄。
     * desc:     ACL 模型描述对象，可读取输入输出尺寸。
     * labels:   该模型对应的标签文本内容。
     * label_count: 实际读取到的标签数量。
     * stage2_index: 如果这是病害模型，则指向 g_stage2_defs 下标；植物模型为 -1。
     */
    uint32_t model_id;
    aclmdlDesc *desc;
    char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN];
    size_t label_count;
    int stage2_index;  /* 指向 g_stage2_defs 的下标；植物主模型固定为 -1。 */
} heshi_model_slot_t;

/* 通用等待参数：
 * 1. 大部分“轮询等待”都以 100ms 为步进。
 * 2. 媒体重载脚本最多等 30 秒。
 * 3. 超时后先给 5 秒 SIGTERM 缓冲，再考虑 SIGKILL。
 */
#define HESHI_WAIT_STEP_US (100 * 1000)
#define HESHI_LOAD_TIMEOUT_MS 30000
#define HESHI_LOAD_TERM_GRACE_MS 5000
#define HESHI_ATTACH_VPSS_FRAME_TIMEOUT_MS 2000

typedef struct {
    /* dataset: ACL 数据集对象。
     * data:    对应的 device memory 指针。
     * size:    这块 device memory 的字节大小。
     *
     * 这个结构体的目的是把“ACL dataset + 底层显存”绑在一起管理，
     * 这样释放资源时不容易漏掉任一部分。
     */
    aclmdlDataset *dataset;
    void *data;
    size_t size;
} heshi_dataset_t;

typedef struct {
    /* class_id: 类别下标。
     * logit:    softmax 前的原始输出值。
     * prob:     softmax 后的概率值。
     */
    int class_id;
    float logit;
    float prob;
} heshi_topk_item_t;

typedef struct {
    /* rgb 指向抓帧后转换得到的整张 RGB 图像缓存。 */
    uint8_t *rgb;
    int width;
    int height;
} heshi_rgb_image_t;

typedef struct {
    /* 这一组标志位用于“按相反顺序安全释放媒体资源”。
     * 某一步启动失败时，可以根据这些标志知道哪些资源已经启动成功。
     */
    td_bool sys_started;
    td_bool vi_started[HESHI_CAMERA_NUM];
    td_bool vi_bound[HESHI_CAMERA_NUM];
    td_bool vpss_started[HESHI_CAMERA_NUM];
    td_bool vo_started;
    sample_vi_cfg vi_cfg[HESHI_CAMERA_NUM];
    ot_vpss_grp vpss_grp[HESHI_CAMERA_NUM];
    ot_size in_size;
} heshi_camera_pipeline_t;

typedef struct {
    /* 这是对“一轮推理结论”的轻量摘要：
     * top1_id/top1_label/top1_prob 描述分类结果，
     * need_alert 表示这一轮在业务上是否已经满足告警条件。
     */
    int top1_id;
    float top1_prob;
    td_bool need_alert;
    char top1_label[HESHI_LABEL_LEN];
} heshi_result_summary_t;

/* g_stop_flag: 收到 SIGINT/SIGTERM 后置 1，让主循环和等待逻辑尽快退出。
 * g_sht3x_*_buf: 直接保存成字符串，是为了写 JSON 时能方便输出 null / 数值文本。
 */
static volatile sig_atomic_t g_stop_flag = 0;
static char g_sht3x_temp_buf[32] = "null";
static char g_sht3x_rh_buf[32] = "null";

/* 每轮推理后读一次温湿度，并缓存为 JSON 可直接写入的字符串形式。 */
static void heshi_read_sht3x(void)
{
    float t, h;
    if (sht3x_is_open()) {
        if (sht3x_read(&t, &h) == 0) {
            snprintf(g_sht3x_temp_buf, sizeof(g_sht3x_temp_buf), "%.2f", (double)t);
            snprintf(g_sht3x_rh_buf, sizeof(g_sht3x_rh_buf), "%.2f", (double)h);
            printf("sht3x: t=%.2f h=%.2f\n", (double)t, (double)h);
        } else {
            fprintf(stderr, "sht3x: read failed\n");
        }
    }
}

/* 把长时间等待拆成多个 100ms 小片段，便于收到退出信号后快速结束。 */
static void heshi_msleep_interruptible(int total_ms)
{
    int slept_ms = 0;

    while (slept_ms < total_ms && g_stop_flag == 0) {
        usleep(HESHI_WAIT_STEP_US);
        slept_ms += HESHI_WAIT_STEP_US / 1000;
    }
}

/* 带超时等待子进程退出，供媒体栈恢复流程使用。 */
static int heshi_wait_pid_exit(pid_t pid, int timeout_ms, int *status_out)
{
    int elapsed_ms = 0;
    int status = 0;
    pid_t wait_ret;

    while (elapsed_ms <= timeout_ms) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            if (status_out != NULL) {
                *status_out = status;
            }
            return 1;
        }
        if (wait_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                return 1;
            }
            fprintf(stderr, "waitpid(%d) failed, errno=%d\n", (int)pid, errno);
            return -1;
        }
        if (elapsed_ms >= timeout_ms) {
            break;
        }
        usleep(HESHI_WAIT_STEP_US);
        elapsed_ms += HESHI_WAIT_STEP_US / 1000;
    }

    return 0;
}

/* 调起板卡提供的媒体初始化脚本，必要时强制回收卡死脚本。 */
static int heshi_reset_media_stack(void)
{
    int elapsed_ms = 0;
    int log_fd;
    int status = 0;
    int wait_ret;
    pid_t pid;
    pid_t wait_pid;

    printf("reset media stack via %s/%s (timeout=%d ms)\n",
        DEFAULT_LOAD_DIR, DEFAULT_LOAD_SCRIPT, HESHI_LOAD_TIMEOUT_MS);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork load script failed, errno=%d\n", errno);
        return -1;
    }

    if (pid == 0) {
        /* child: detach from terminal, run load script */
        (void)setpgid(0, 0);
        log_fd = open(DEFAULT_LOAD_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            (void)dup2(log_fd, STDOUT_FILENO);
            (void)dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        if (chdir(DEFAULT_LOAD_DIR) != 0) {
            fprintf(stderr, "chdir %s failed, errno=%d\n", DEFAULT_LOAD_DIR, errno);
            _exit(127);
        }
        execl(DEFAULT_LOAD_SCRIPT, DEFAULT_LOAD_SCRIPT,
            "-a",
            "-sensor0", "os08a20",
            "-sensor1", "os08a20",
            "-sensor2", "os08a20",
            "-sensor3", "os08a20",
            (char *)NULL);
        fprintf(stderr, "exec %s/%s failed, errno=%d\n", DEFAULT_LOAD_DIR, DEFAULT_LOAD_SCRIPT, errno);
        _exit(127);
    }

    /* parent: wait with timeout, kill process group on timeout */
    (void)setpgid(pid, pid);
    wait_ret = 0;
    while (elapsed_ms <= HESHI_LOAD_TIMEOUT_MS) {
        wait_pid = waitpid(pid, &status, WNOHANG);
        if (wait_pid == pid) {
            wait_ret = 1;
            break;
        }
        if (wait_pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                wait_ret = 1;
                break;
            }
            fprintf(stderr, "wait load script failed, errno=%d\n", errno);
            wait_ret = -1;
            break;
        }
        if (g_stop_flag == 1 || elapsed_ms >= HESHI_LOAD_TIMEOUT_MS) {
            break;
        }
        usleep(HESHI_WAIT_STEP_US);
        elapsed_ms += HESHI_WAIT_STEP_US / 1000;
    }

    if (wait_ret == 0) {
        fprintf(stderr, "load script timeout after %d ms, killing process group; check %s\n",
            HESHI_LOAD_TIMEOUT_MS, DEFAULT_LOAD_LOG);
        (void)kill(-pid, SIGTERM);
        wait_ret = heshi_wait_pid_exit(pid, HESHI_LOAD_TERM_GRACE_MS, &status);
        if (wait_ret == 0) {
            (void)kill(-pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
        }
        return -1;
    }
    if (wait_ret < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "load script killed by signal=%d, check %s\n",
                WTERMSIG(status), DEFAULT_LOAD_LOG);
        } else if (WIFEXITED(status)) {
            fprintf(stderr, "load script failed, status=%d, check %s\n",
                WEXITSTATUS(status), DEFAULT_LOAD_LOG);
        } else {
            fprintf(stderr, "load script failed, check %s\n", DEFAULT_LOAD_LOG);
        }
        return -1;
    }

    sleep(2);
    return 0;
}

static void print_usage(const char *argv0)
{
    printf("Usage: %s [model.om] [labels.txt] [result.json] [frame.ppm] [camera_id] [device_id] [loop_count] [interval_sec] [center_crop_ratio] [--attach-only] [--reload-media]\n", argv0);
    printf("\nDefault: start built-in single-camera VI/VPSS pipeline internally.\n");
    printf("  loop_count: 1 for one-shot, 0 for infinite loop (default).\n");
    printf("  --attach-only: attach to already-running VPSS (debug only).\n");
    printf("  --reload-media: recovery mode, run load_ss928v100 before starting.\n");
    printf("\nDefaults:\n");
    printf("  model    : %s\n", DEFAULT_MODEL_PATH);
    printf("  labels   : %s\n", DEFAULT_LABELS_PATH);
    printf("  json     : %s\n", DEFAULT_JSON_PATH);
    printf("  frame ppm: %s\n", DEFAULT_PPM_PATH);
    printf("  camera   : %d (camera 0 only)\n", DEFAULT_CAMERA_ID);
    printf("  device_id: %s\n", DEFAULT_DEVICE_ID);
    printf("  loop_cnt : 0 (infinite)\n");
    printf("  interval : %d sec\n", DEFAULT_INTERVAL_SEC);
    printf("  crop     : %.2f (center crop ratio before resize)\n", DEFAULT_CENTER_CROP_RATIO);
}

static void heshi_signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        g_stop_flag = 1;
    }
}

/* 从标签文本读取类别名，一行对应一个类别。 */
static size_t load_labels(const char *path, char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN])
{
    FILE *fp = fopen(path, "r");
    size_t count = 0;

    if (fp == NULL) {
        fprintf(stderr, "labels not loaded: %s, errno=%d\n", path, errno);
        return 0;
    }

    while (count < HESHI_MAX_LABELS && fgets(labels[count], HESHI_LABEL_LEN, fp) != NULL) {
        size_t len = strlen(labels[count]);
        while (len > 0 && (labels[count][len - 1] == '\n' || labels[count][len - 1] == '\r')) {
            labels[count][len - 1] = '\0';
            len--;
        }
        if (len > 0) {
            count++;
        }
    }

    fclose(fp);
    return count;
}

/* 取得类别名；如果标签缺失则返回 class_xxx 形式的兜底名字。 */
static const char *label_name(char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN], size_t label_count,
    size_t index, char *fallback, size_t fallback_len)
{
    if (index < label_count) {
        return labels[index];
    }
    (void)snprintf(fallback, fallback_len, "class_%zu", index);
    return fallback;
}

/* 健康类或背景类不会触发告警，这里统一归一到“无需告警”的判断。 */
static td_bool heshi_is_healthy_like(const char *label)
{
    if (label == TD_NULL) {
        return TD_FALSE;
    }
    if (strstr(label, "healthy") != TD_NULL) {
        return TD_TRUE;
    }
    if (strcmp(label, "Unknown_or_Background") == 0) {
        return TD_TRUE;
    }
    return TD_FALSE;
}

/* 限幅到 8bit 图像范围。 */
static td_u8 heshi_clip(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (td_u8)value;
}

/* 把一组 NV21 的 Y/V/U 采样值转换成一个 RGB 像素。 */
static td_void heshi_nv21_to_rgb_pixel(td_u8 y, td_u8 v, td_u8 u, td_u8 *rgb)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    int r;
    int g;
    int b;

    if (c < 0) {
        c = 0;
    }

    r = (298 * c + 409 * e + 128) >> 8;
    g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b = (298 * c + 516 * d + 128) >> 8;

    rgb[0] = heshi_clip(r);
    rgb[1] = heshi_clip(g);
    rgb[2] = heshi_clip(b);
}

/*
 * 仅在 attach-only 模式下使用：
 * 把外部已启动的 VPSS 通道修正为线性、非压缩、可抓帧的状态，
 * 否则后续抓图和 RGB 转换可能失败。
 */
static td_s32 heshi_prepare_attach_vpss(ot_vpss_grp grp, ot_vpss_chn chn)
{
    td_s32 ret;
    ot_vpss_chn_attr chn_attr;

    (td_void)memset_s(&chn_attr, sizeof(chn_attr), 0, sizeof(chn_attr));
    ret = ss_mpi_vpss_get_chn_attr(grp, chn, &chn_attr);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_vpss_get_chn_attr failed, ret=0x%x\n", ret);
        return ret;
    }

    chn_attr.depth = 3;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;

    ret = ss_mpi_vpss_set_chn_attr(grp, chn, &chn_attr);
    if (ret != TD_SUCCESS) {
        sample_print("ss_mpi_vpss_set_chn_attr failed, ret=0x%x\n", ret);
        return ret;
    }

    return TD_SUCCESS;
}

/* 生成系统公共视频缓冲池配置。 */
static td_void heshi_get_default_vb_config(const ot_size *size, ot_vb_cfg *vb_cfg,
    ot_vi_video_mode video_mode, td_u32 yuv_cnt, td_u32 raw_cnt)
{
    ot_vb_calc_cfg calc_cfg;
    ot_pic_buf_attr buf_attr;

    (td_void)memset_s(vb_cfg, sizeof(*vb_cfg), 0, sizeof(*vb_cfg));
    vb_cfg->max_pool_cnt = 128;

    buf_attr.width = size->width;
    buf_attr.height = size->height;
    buf_attr.align = OT_DEFAULT_ALIGN;
    buf_attr.bit_width = OT_DATA_BIT_WIDTH_8;
    buf_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    buf_attr.compress_mode = OT_COMPRESS_MODE_SEG;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg->common_pool[0].blk_size = calc_cfg.vb_size;
    vb_cfg->common_pool[0].blk_cnt = yuv_cnt;

    buf_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    buf_attr.compress_mode = (video_mode == OT_VI_VIDEO_MODE_NORM) ? OT_COMPRESS_MODE_LINE : OT_COMPRESS_MODE_NONE;
    ot_common_get_pic_buf_cfg(&buf_attr, &calc_cfg);
    vb_cfg->common_pool[1].blk_size = calc_cfg.vb_size;
    vb_cfg->common_pool[1].blk_cnt = raw_cnt;
}

/* 初始化媒体系统和 VI/VPSS 连接模式。 */
static td_s32 heshi_sys_init_for_dual_camera(const ot_size *size)
{
    td_s32 ret;
    ot_vb_cfg vb_cfg;
    const ot_vi_vpss_mode_type mode_type = OT_VI_OFFLINE_VPSS_OFFLINE;
    const ot_vi_video_mode video_mode = OT_VI_VIDEO_MODE_NORM;

    heshi_get_default_vb_config(size, &vb_cfg, video_mode, HESHI_VB_DOUBLE_YUV_CNT, HESHI_VB_WDR_RAW_CNT);

    ret = sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK);
    if (ret != TD_SUCCESS) {
        /*
         * If the previous run crashed or VB pools are stale, try a cleanup
         * and one retry before failing.  sample_comm_sys_init_with_vb_supplement
         * already calls ss_mpi_sys_exit + ss_mpi_vb_exit internally, but call
         * them again here just in case.
         */
        printf("sys_init failed (ret=0x%x), attempting cleanup and retry...\n", ret);
        ss_mpi_sys_exit();
        ss_mpi_vb_exit();
        usleep(500 * 1000);

        ret = sample_comm_sys_init_with_vb_supplement(&vb_cfg, OT_VB_SUPPLEMENT_BNR_MOT_MASK);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_sys_init_with_vb_supplement failed after retry, ret=0x%x\n", ret);
            sample_print("You may need to run with --reload-media to reset all media modules.\n");
            return ret;
        }
    }

    ret = sample_comm_vi_set_vi_vpss_mode(mode_type, video_mode);
    if (ret != TD_SUCCESS) {
        sample_print("sample_comm_vi_set_vi_vpss_mode failed, ret=0x%x\n", ret);
        sample_comm_sys_exit();
        return ret;
    }

    return TD_SUCCESS;
}

static td_void heshi_get_sensor0_vi_cfg(sample_sns_type sns_type, sample_vi_cfg *vi_cfg)
{
    sample_comm_vi_get_default_vi_cfg(sns_type, vi_cfg);
}

/* 启动一个 VPSS 组，并把通道 0 配成适合抓帧和显示的输出。 */
static td_s32 heshi_start_vpss(ot_vpss_grp grp, const ot_size *size)
{
    td_s32 ret;
    td_bool chn_enable[OT_VPSS_MAX_PHYS_CHN_NUM] = {TD_TRUE, TD_FALSE, TD_FALSE, TD_FALSE};
    ot_vpss_grp_attr grp_attr;
    ot_vpss_chn_attr chn_attr[OT_VPSS_MAX_PHYS_CHN_NUM];

    (td_void)memset_s(chn_attr, sizeof(chn_attr), 0, sizeof(chn_attr));
    sample_comm_vpss_get_default_grp_attr(&grp_attr);
    grp_attr.max_width = size->width;
    grp_attr.max_height = size->height;

    sample_comm_vpss_get_default_chn_attr(&chn_attr[0]);
    chn_attr[0].width = size->width;
    chn_attr[0].height = size->height;
    chn_attr[0].depth = 3;

    ret = sample_common_vpss_start(grp, chn_enable, &grp_attr, chn_attr, OT_VPSS_MAX_PHYS_CHN_NUM);
    if (ret != TD_SUCCESS) {
        sample_print("sample_common_vpss_start grp=%d failed, ret=0x%x\n", grp, ret);
        return ret;
    }

    /* 低时延设置能更快拿到新帧，代价是可能轻微牺牲一些画质。 */
    {
        ot_low_delay_info low_delay_info;
        low_delay_info.enable = TD_TRUE;
        low_delay_info.line_cnt = 200;
        low_delay_info.one_buf_en = TD_FALSE;
        ret = ss_mpi_vpss_set_low_delay_attr(grp, 0, &low_delay_info);
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_vpss_set_low_delay_attr grp=%d failed, ret=0x%x\n", grp, ret);
            sample_common_vpss_stop(grp, chn_enable, OT_VPSS_MAX_PHYS_CHN_NUM);
            return ret;
        }
    }

    return TD_SUCCESS;
}

static td_void heshi_stop_vpss(ot_vpss_grp grp)
{
    td_bool chn_enable[OT_VPSS_MAX_PHYS_CHN_NUM] = {TD_TRUE, TD_FALSE, TD_FALSE, TD_FALSE};

    (td_void)sample_common_vpss_stop(grp, chn_enable, OT_VPSS_MAX_PHYS_CHN_NUM);
}

/* 启动 HDMI 输出，方便现场观察取景和管线是否正常。 */
static td_s32 heshi_start_vo(heshi_camera_pipeline_t *pipeline)
{
    td_s32 ret;
    sample_vo_cfg vo_cfg;
    const ot_vo_layer vo_layer = 0;
    ot_vo_chn vo_chn[HESHI_CAMERA_NUM] = {0};
    int i;

    sample_comm_vo_get_def_config(&vo_cfg);
    vo_cfg.vo_dev       = SAMPLE_VO_DEV_UHD;
    vo_cfg.vo_intf_type = OT_VO_INTF_HDMI;
    vo_cfg.intf_sync    = OT_VO_OUT_1080P30;
    vo_cfg.pix_format   = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    vo_cfg.disp_rect    = (ot_rect){0, 0, 1920, 1080};
    vo_cfg.image_size   = (ot_size){1920, 1080};
    vo_cfg.vo_mode      = VO_MODE_1MUX;
    vo_cfg.compress_mode = OT_COMPRESS_MODE_NONE;

    ret = sample_comm_vo_start_vo(&vo_cfg);
    if (ret != TD_SUCCESS) {
        sample_print("sample_comm_vo_start_vo failed, ret=0x%x\n", ret);
        return ret;
    }

    for (i = 0; i < HESHI_CAMERA_NUM; i++) {
        ret = sample_comm_vpss_bind_vo(pipeline->vpss_grp[i], DEFAULT_VPSS_CHN, vo_layer, vo_chn[i]);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_vpss_bind_vo grp=%d failed, ret=0x%x\n", pipeline->vpss_grp[i], ret);
            for (i = i - 1; i >= 0; i--) {
                sample_comm_vpss_un_bind_vo(pipeline->vpss_grp[i], DEFAULT_VPSS_CHN, vo_layer, vo_chn[i]);
            }
            sample_comm_vo_stop_vo(&vo_cfg);
            return ret;
        }
    }

    pipeline->vo_started = TD_TRUE;
    printf("VO started, HDMI 1080P30 1MUX, vpss grp %d -> vo chn 0\n",
        pipeline->vpss_grp[0]);
    return TD_SUCCESS;
}

static td_void heshi_stop_vo(heshi_camera_pipeline_t *pipeline)
{
    sample_vo_cfg vo_cfg;
    const ot_vo_layer vo_layer = 0;
    ot_vo_chn vo_chn[HESHI_CAMERA_NUM] = {0};
    int i;

    for (i = 0; i < HESHI_CAMERA_NUM; i++) {
        sample_comm_vpss_un_bind_vo(pipeline->vpss_grp[i], DEFAULT_VPSS_CHN, vo_layer, vo_chn[i]);
    }

    sample_comm_vo_get_def_config(&vo_cfg);
    vo_cfg.vo_dev       = SAMPLE_VO_DEV_UHD;
    vo_cfg.vo_intf_type = OT_VO_INTF_HDMI;
    vo_cfg.intf_sync    = OT_VO_OUT_1080P30;
    vo_cfg.vo_mode      = VO_MODE_1MUX;
    sample_comm_vo_stop_vo(&vo_cfg);

    pipeline->vo_started = TD_FALSE;
    printf("VO stopped\n");
}

/* 启动单摄像头媒体管线：VI -> VPSS -> 可选 VO。 */
static td_s32 heshi_start_camera_pipeline(heshi_camera_pipeline_t *pipeline)
{
    td_s32 ret;
    sample_sns_type sns_type = SENSOR0_TYPE;
    int i;

    (td_void)memset_s(pipeline, sizeof(*pipeline), 0, sizeof(*pipeline));
    pipeline->vpss_grp[0] = 0;

    sample_comm_vi_get_size_by_sns_type(sns_type, &pipeline->in_size);
    printf("start built-in single camera pipeline, sensor=%d size=%ux%u\n",
        sns_type, pipeline->in_size.width, pipeline->in_size.height);

    ret = heshi_sys_init_for_dual_camera(&pipeline->in_size);
    if (ret != TD_SUCCESS) {
        return ret;
    }
    pipeline->sys_started = TD_TRUE;

    heshi_get_sensor0_vi_cfg(sns_type, &pipeline->vi_cfg[0]);
    for (i = 0; i < HESHI_CAMERA_NUM; i++) {
        ret = sample_comm_vi_start_vi(&pipeline->vi_cfg[i]);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_vi_start_vi camera=%d failed, ret=0x%x\n", i, ret);
            return ret;
        }
        pipeline->vi_started[i] = TD_TRUE;
    }

    for (i = 0; i < HESHI_CAMERA_NUM; i++) {
        ret = sample_comm_vi_bind_vpss((ot_vi_pipe)i, 0, pipeline->vpss_grp[i], DEFAULT_VPSS_CHN);
        if (ret != TD_SUCCESS) {
            sample_print("sample_comm_vi_bind_vpss camera=%d failed, ret=0x%x\n", i, ret);
            return ret;
        }
        pipeline->vi_bound[i] = TD_TRUE;
    }

    for (i = 0; i < HESHI_CAMERA_NUM; i++) {
        ret = heshi_start_vpss(pipeline->vpss_grp[i], &pipeline->in_size);
        if (ret != TD_SUCCESS) {
            return ret;
        }
        pipeline->vpss_started[i] = TD_TRUE;
    }

    /* Start VO (HDMI 1080P30 1MUX) and bind VPSS group */
    ret = heshi_start_vo(pipeline);
    if (ret != TD_SUCCESS) {
        sample_print("VO start failed, continuing without display\n");
        /* non-fatal: inference still works without HDMI output */
    }

    printf("built-in single camera pipeline started\n");
    return TD_SUCCESS;
}

/* 关闭媒体管线，顺序与启动相反，避免残留绑定关系。 */
static td_void heshi_stop_camera_pipeline(heshi_camera_pipeline_t *pipeline)
{
    int i;

    /* Stop VO first (unbind VPSS->VO, stop HDMI) */
    if (pipeline->vo_started == TD_TRUE) {
        heshi_stop_vo(pipeline);
    }

    /* Stop VPSS to stop frames flowing */
    for (i = HESHI_CAMERA_NUM - 1; i >= 0; i--) {
        if (pipeline->vpss_started[i] == TD_TRUE) {
            heshi_stop_vpss(pipeline->vpss_grp[i]);
            pipeline->vpss_started[i] = TD_FALSE;
        }
    }

    /* Unbind VI from VPSS */
    for (i = HESHI_CAMERA_NUM - 1; i >= 0; i--) {
        if (pipeline->vi_bound[i] == TD_TRUE) {
            (td_void)sample_comm_vi_un_bind_vpss((ot_vi_pipe)i, 0, pipeline->vpss_grp[i], DEFAULT_VPSS_CHN);
            pipeline->vi_bound[i] = TD_FALSE;
        }
    }

    /* Stop VI / ISP */
    for (i = HESHI_CAMERA_NUM - 1; i >= 0; i--) {
        if (pipeline->vi_started[i] == TD_TRUE) {
            sample_comm_vi_stop_vi(&pipeline->vi_cfg[i]);
            pipeline->vi_started[i] = TD_FALSE;
        }
    }

    /* Finalize system / VB */
    if (pipeline->sys_started == TD_TRUE) {
        sample_comm_sys_exit();
        pipeline->sys_started = TD_FALSE;
    }
    printf("built-in single camera pipeline stopped\n");
}

/* 从 VPSS 抓一帧，转成 RGB，并顺手保存为 PPM 供调试和告警缩略图使用。 */
static td_s32 heshi_capture_frame_rgb(ot_vpss_grp grp, ot_vpss_chn chn, const char *ppm_path,
    ot_video_frame_info *frame_info, heshi_rgb_image_t *rgb_image)
{
    /* y_plane_size / uv_plane_size: NV21 两个平面的总字节数。
     * y_base / uv_base:             mmap 后得到的 CPU 可访问地址。
     * rgb_buf:                      最终生成的整张 RGB 图像缓存。
     * fp:                           用来把 RGB 图保存为 PPM 的文件句柄。
     */
    td_s32 ret;
    td_u32 y_plane_size;
    td_u32 uv_plane_size;
    td_u8 *y_base;
    td_u8 *uv_base;
    td_u8 *rgb_buf = TD_NULL;
    td_u32 x;
    td_u32 y;
    FILE *fp = TD_NULL;

    /* 先清空输出结构，避免调用失败时残留旧帧信息。 */
    memset(frame_info, 0, sizeof(*frame_info));
    {
        int tries;
        /* first frame after VPSS start may need a few attempts */
        for (tries = 0; tries < 5; tries++) {
            ret = ss_mpi_vpss_get_chn_frame(grp, chn, frame_info, 400);
            if (ret == TD_SUCCESS) break;
            if (g_stop_flag == 1) return ret;
        }
        if (ret != TD_SUCCESS) {
            sample_print("ss_mpi_vpss_get_chn_frame failed, ret=0x%x\n", ret);
            return ret;
        }
    }

    printf("captured frame: %ux%u strideY=%u strideUV=%u fmt=%d vf=%d comp=%d\n",
        frame_info->video_frame.width, frame_info->video_frame.height,
        frame_info->video_frame.stride[0], frame_info->video_frame.stride[1],
        frame_info->video_frame.pixel_format, frame_info->video_frame.video_format,
        frame_info->video_frame.compress_mode);

    if (frame_info->video_frame.pixel_format != OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420 ||
        frame_info->video_frame.video_format != OT_VIDEO_FORMAT_LINEAR) {
        sample_print("unsupported frame format for RGB convert\n");
        ss_mpi_vpss_release_chn_frame(grp, chn, frame_info);
        return TD_FAILURE;
    }

    /* stride 可能大于 width，所以计算平面大小必须用 stride。 */
    y_plane_size = frame_info->video_frame.stride[0] * frame_info->video_frame.height;
    uv_plane_size = frame_info->video_frame.stride[1] * (frame_info->video_frame.height / 2);
    y_base = (td_u8 *)ss_mpi_sys_mmap(frame_info->video_frame.phys_addr[0], y_plane_size);
    uv_base = (td_u8 *)ss_mpi_sys_mmap(frame_info->video_frame.phys_addr[1], uv_plane_size);
    if (y_base == TD_NULL || uv_base == TD_NULL) {
        sample_print("ss_mpi_sys_mmap failed for frame\n");
        if (y_base != TD_NULL) {
            ss_mpi_sys_munmap(y_base, y_plane_size);
        }
        if (uv_base != TD_NULL) {
            ss_mpi_sys_munmap(uv_base, uv_plane_size);
        }
        ss_mpi_vpss_release_chn_frame(grp, chn, frame_info);
        return TD_FAILURE;
    }

    rgb_buf = (td_u8 *)malloc(frame_info->video_frame.width * frame_info->video_frame.height * 3);
    if (rgb_buf == TD_NULL) {
        sample_print("malloc rgb buffer failed\n");
        ss_mpi_sys_munmap(y_base, y_plane_size);
        ss_mpi_sys_munmap(uv_base, uv_plane_size);
        ss_mpi_vpss_release_chn_frame(grp, chn, frame_info);
        return TD_FAILURE;
    }

    /* 逐像素把 NV21 转成 RGB。
     * 注意 UV 平面是 2x2 采样共享，所以 (x/2)*2 取到同一组 U/V。
     */
    for (y = 0; y < frame_info->video_frame.height; y++) {
        td_u8 *y_row = y_base + y * frame_info->video_frame.stride[0];
        td_u8 *uv_row = uv_base + (y / 2) * frame_info->video_frame.stride[1];
        for (x = 0; x < frame_info->video_frame.width; x++) {
            td_u8 v = uv_row[(x / 2) * 2];
            td_u8 u = uv_row[(x / 2) * 2 + 1];
            heshi_nv21_to_rgb_pixel(y_row[x], v, u, &rgb_buf[(y * frame_info->video_frame.width + x) * 3]);
        }
    }

    fp = fopen(ppm_path, "wb");
    if (fp == TD_NULL) {
        sample_print("open ppm output failed: %s errno=%d\n", ppm_path, errno);
        free(rgb_buf);
        ss_mpi_sys_munmap(y_base, y_plane_size);
        ss_mpi_sys_munmap(uv_base, uv_plane_size);
        ss_mpi_vpss_release_chn_frame(grp, chn, frame_info);
        return TD_FAILURE;
    }

    fprintf(fp, "P6\n%u %u\n255\n", frame_info->video_frame.width, frame_info->video_frame.height);
    fwrite(rgb_buf, 1, frame_info->video_frame.width * frame_info->video_frame.height * 3, fp);
    fclose(fp);
    printf("saved ppm to %s\n", ppm_path);

    /* 这里把 RGB 缓冲交给调用方继续用于预处理，所以不能在本函数里 free。 */
    rgb_image->rgb = rgb_buf;
    rgb_image->width = (int)frame_info->video_frame.width;
    rgb_image->height = (int)frame_info->video_frame.height;

    ss_mpi_sys_munmap(y_base, y_plane_size);
    ss_mpi_sys_munmap(uv_base, uv_plane_size);
    ss_mpi_vpss_release_chn_frame(grp, chn, frame_info);
    return TD_SUCCESS;
}

/* 双线性插值采样，用于缩放 RGB 图像。 */
static uint8_t bilinear_sample(const uint8_t *src, int src_w, int src_h, float src_x, float src_y, int channel)
{
    int x0 = (int)floorf(src_x);
    int y0 = (int)floorf(src_y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float wx = src_x - (float)x0;
    float wy = src_y - (float)y0;
    float v00;
    float v01;
    float v10;
    float v11;
    float top;
    float bottom;
    float value;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= src_w) {
        x1 = src_w - 1;
    }
    if (y1 >= src_h) {
        y1 = src_h - 1;
    }

    v00 = src[((size_t)y0 * src_w + x0) * 3 + channel];
    v01 = src[((size_t)y0 * src_w + x1) * 3 + channel];
    v10 = src[((size_t)y1 * src_w + x0) * 3 + channel];
    v11 = src[((size_t)y1 * src_w + x1) * 3 + channel];
    top = v00 + (v01 - v00) * wx;
    bottom = v10 + (v11 - v10) * wx;
    value = top + (bottom - top) * wy;

    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 255.0f) {
        value = 255.0f;
    }
    return (uint8_t)(value + 0.5f);
}

/* 预处理流程与训练保持一致：中心裁剪 -> 缩放 -> /255 -> HWC 转 NCHW。 */
static int preprocess_rgb_to_nchw(const heshi_rgb_image_t *rgb_image, int dst_w, int dst_h,
    float center_crop_ratio, uint8_t **out_buf, size_t *out_size)
{
    /* resized: 缩放后的中间 RGB 图像。
     * nchw:    最终送给模型的 float32 输入缓存。
     *
     * src_crop_* 描述从原图里裁哪一块。
     * resize_*   描述裁完之后先缩放到多大。
     * crop_x/y   描述如果缩放后不是正方形，最终再从中间裁出 dst_w x dst_h。
     */
    uint8_t *resized = NULL;
    float *nchw = NULL;
    int resize_short;
    int resize_w;
    int resize_h;
    int crop_x;
    int crop_y;
    int src_crop_x = 0;
    int src_crop_y = 0;
    int src_crop_w = rgb_image->width;
    int src_crop_h = rgb_image->height;
    int x;
    int y;
    int c;

    if (dst_w <= 0 || dst_h <= 0 || dst_w != dst_h) {
        fprintf(stderr, "unsupported model image size: %dx%d\n", dst_w, dst_h);
        return -1;
    }

    if (center_crop_ratio > 0.0f && center_crop_ratio < 1.0f) {
        /* 用户显式给了裁剪比例时，按比例截取原图中心区域。 */
        src_crop_w = (int)((float)rgb_image->width * center_crop_ratio);
        src_crop_h = (int)((float)rgb_image->height * center_crop_ratio);
        if (src_crop_w < dst_w) {
            src_crop_w = dst_w;
        }
        if (src_crop_h < dst_h) {
            src_crop_h = dst_h;
        }
        src_crop_x = (rgb_image->width - src_crop_w) / 2;
        src_crop_y = (rgb_image->height - src_crop_h) / 2;
    } else {
        /* 默认策略：
         * 先用短边做一个正方形中心裁剪，再缩放到模型输入大小。
         * 这是最贴近训练数据处理流程的方式。
         */
        int crop_size = (src_crop_w < src_crop_h) ? src_crop_w : src_crop_h;
        src_crop_w = crop_size;
        src_crop_h = crop_size;
        src_crop_x = (rgb_image->width - crop_size) / 2;
        src_crop_y = (rgb_image->height - crop_size) / 2;
    }

    resize_short = dst_w;
    if (src_crop_w < src_crop_h) {
        resize_w = resize_short;
        resize_h = (int)roundf((float)src_crop_h * (float)resize_short / (float)src_crop_w);
    } else {
        resize_h = resize_short;
        resize_w = (int)roundf((float)src_crop_w * (float)resize_short / (float)src_crop_h);
    }

    resized = (uint8_t *)malloc((size_t)resize_w * (size_t)resize_h * 3);
    if (resized == NULL) {
        fprintf(stderr, "malloc resized image failed\n");
        return -1;
    }

    for (y = 0; y < resize_h; y++) {
        float src_y = ((float)y + 0.5f) * (float)src_crop_h / (float)resize_h - 0.5f + (float)src_crop_y;
        for (x = 0; x < resize_w; x++) {
            float src_x = ((float)x + 0.5f) * (float)src_crop_w / (float)resize_w - 0.5f + (float)src_crop_x;
            for (c = 0; c < 3; c++) {
                resized[((size_t)y * resize_w + x) * 3 + c] =
                    bilinear_sample(rgb_image->rgb, rgb_image->width, rgb_image->height, src_x, src_y, c);
            }
        }
    }

    nchw = (float *)malloc((size_t)3 * (size_t)dst_h * (size_t)dst_w * sizeof(float));
    if (nchw == NULL) {
        fprintf(stderr, "malloc nchw input failed\n");
        free(resized);
        return -1;
    }

    /* 如果 resize 后某一边大于目标尺寸，则从中心再裁一刀。 */
    crop_x = (resize_w - dst_w) / 2;
    crop_y = (resize_h - dst_h) / 2;
    for (c = 0; c < 3; c++) {
        for (y = 0; y < dst_h; y++) {
            for (x = 0; x < dst_w; x++) {
                uint8_t pixel = resized[((size_t)(y + crop_y) * resize_w + (x + crop_x)) * 3 + c];
                nchw[(size_t)c * dst_h * dst_w + (size_t)y * dst_w + x] = (float)pixel / 255.0f;
            }
        }
    }

    printf("preprocess rgb: src=%dx%d center_crop=%dx%d@(%d,%d) resize=%dx%d crop=%dx%d\n",
        rgb_image->width, rgb_image->height, src_crop_w, src_crop_h, src_crop_x, src_crop_y,
        resize_w, resize_h, dst_w, dst_h);

    free(resized);
    *out_buf = (uint8_t *)nchw;
    *out_size = (size_t)3 * (size_t)dst_h * (size_t)dst_w * sizeof(float);
    return 0;
}

/* 对原始 logits 做 softmax，转成概率分布。 */
static void softmax(const float *src, float *dst, size_t count)
{
    size_t i;
    float max_v = src[0];
    double sum = 0.0;

    for (i = 1; i < count; i++) {
        if (src[i] > max_v) {
            max_v = src[i];
        }
    }

    for (i = 0; i < count; i++) {
        dst[i] = (float)exp((double)src[i] - (double)max_v);
        sum += dst[i];
    }

    if (sum <= 0.0) {
        return;
    }

    for (i = 0; i < count; i++) {
        dst[i] = (float)((double)dst[i] / sum);
    }
}

/* 从全量类别中挑出 top-k，便于日志打印和结果落盘。 */
static void compute_topk(const float *logits, size_t count, const float *prob, heshi_topk_item_t topk[HESHI_TOPK])
{
    size_t i;
    int k;

    for (k = 0; k < HESHI_TOPK; k++) {
        topk[k].class_id = -1;
        topk[k].logit = 0.0f;
        topk[k].prob = 0.0f;
    }

    for (i = 0; i < count; i++) {
        for (k = 0; k < HESHI_TOPK; k++) {
            if (topk[k].class_id < 0 || logits[i] > topk[k].logit) {
                int move;
                for (move = HESHI_TOPK - 1; move > k; move--) {
                    topk[move] = topk[move - 1];
                }
                topk[k].class_id = (int)i;
                topk[k].logit = logits[i];
                topk[k].prob = prob[i];
                break;
            }
        }
    }
}

/* 打印 top-k 结果，方便串口日志查看模型行为。 */
static void print_topk(const heshi_topk_item_t topk[HESHI_TOPK],
    char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN], size_t label_count)
{
    int k;

    printf("\nTop-%d result:\n", HESHI_TOPK);
    for (k = 0; k < HESHI_TOPK; k++) {
        char fallback[32];
        const char *name;
        int idx = topk[k].class_id;
        if (idx < 0) {
            continue;
        }
        name = label_name(labels, label_count, (size_t)idx, fallback, sizeof(fallback));
        printf("  top%d: id=%d, logit=%.8f, prob=%.6f, label=%s\n",
            k, idx, topk[k].logit, topk[k].prob, name);
    }
}

/* 构造 ACL dataset，并把一块 device memory 挂到 dataset 上。 */
static int create_dataset_with_buffer(heshi_dataset_t *dataset, void *dev_buf, size_t size)
{
    aclDataBuffer *data_buffer = NULL;
    aclError ret;

    dataset->dataset = aclmdlCreateDataset();
    if (dataset->dataset == NULL) {
        fprintf(stderr, "aclmdlCreateDataset failed\n");
        return -1;
    }

    data_buffer = aclCreateDataBuffer(dev_buf, size);
    if (data_buffer == NULL) {
        fprintf(stderr, "aclCreateDataBuffer failed\n");
        (void)aclmdlDestroyDataset(dataset->dataset);
        dataset->dataset = NULL;
        return -1;
    }

    ret = aclmdlAddDatasetBuffer(dataset->dataset, data_buffer);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclmdlAddDatasetBuffer failed, ret=%d\n", ret);
        (td_void)aclDestroyDataBuffer(data_buffer);
        (td_void)aclmdlDestroyDataset(dataset->dataset);
        dataset->dataset = NULL;
        return -1;
    }

    dataset->data = dev_buf;
    dataset->size = size;
    return 0;
}

/* 释放 dataset 及其关联的 device memory。 */
static void destroy_dataset(heshi_dataset_t *dataset)
{
    size_t i;
    size_t num;

    if (dataset->dataset == NULL) {
        return;
    }

    num = aclmdlGetDatasetNumBuffers(dataset->dataset);
    for (i = 0; i < num; i++) {
        aclDataBuffer *data_buffer = aclmdlGetDatasetBuffer(dataset->dataset, i);
        if (data_buffer != NULL) {
            (td_void)aclDestroyDataBuffer(data_buffer);
        }
    }

    (td_void)aclmdlDestroyDataset(dataset->dataset);
    dataset->dataset = NULL;

    if (dataset->data != NULL) {
        (td_void)aclrtFree(dataset->data);
        dataset->data = NULL;
    }
    dataset->size = 0;
}

/* 为模型输入准备 device buffer，并把主机侧输入拷进去。 */
static int prepare_input_dataset(aclmdlDesc *desc, uint8_t *host_input, size_t host_input_size, heshi_dataset_t *input)
{
    size_t input_size = aclmdlGetInputSizeByIndex(desc, 0);
    void *dev_input = NULL;
    aclError ret;

    if (host_input_size != input_size) {
        fprintf(stderr, "input size mismatch: host=%zu model=%zu\n", host_input_size, input_size);
        return -1;
    }

    ret = aclrtMalloc(&dev_input, input_size, ACL_MEM_MALLOC_NORMAL_ONLY);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtMalloc input failed, size=%zu, ret=%d\n", input_size, ret);
        return -1;
    }

    ret = aclrtMemcpy(dev_input, input_size, host_input, input_size, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtMemcpy input H2D failed, ret=%d\n", ret);
        (td_void)aclrtFree(dev_input);
        return -1;
    }

    if (create_dataset_with_buffer(input, dev_input, input_size) != 0) {
        (td_void)aclrtFree(dev_input);
        return -1;
    }

    return 0;
}

/* 为模型输出准备 device buffer。 */
static int prepare_output_dataset(aclmdlDesc *desc, heshi_dataset_t *output)
{
    size_t output_size = aclmdlGetOutputSizeByIndex(desc, 0);
    void *dev_output = NULL;
    aclError ret;

    if (output_size == 0) {
        fprintf(stderr, "model output size is 0\n");
        return -1;
    }

    ret = aclrtMalloc(&dev_output, output_size, ACL_MEM_MALLOC_NORMAL_ONLY);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtMalloc output failed, size=%zu, ret=%d\n", output_size, ret);
        return -1;
    }

    if (create_dataset_with_buffer(output, dev_output, output_size) != 0) {
        (td_void)aclrtFree(dev_output);
        return -1;
    }

    return 0;
}

/* 把当前这一轮推理结果写到 JSON，供本地和外部程序读取。 */
static int write_result_json(const char *json_path, const char *device_id, const char *ppm_path, int camera_id,
    td_bool need_alert, int consecutive_hits, const heshi_topk_item_t topk[HESHI_TOPK],
    char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN], size_t label_count)
{
    FILE *fp;
    struct timeval tv;
    int k;
    char fallback[32];
    const char *top1_name = "unknown";

    if (json_path == NULL || json_path[0] == '\0') {
        return 0;
    }

    if (topk[0].class_id >= 0) {
        top1_name = label_name(labels, label_count, (size_t)topk[0].class_id, fallback, sizeof(fallback));
    }

    fp = fopen(json_path, "w");
    if (fp == NULL) {
        fprintf(stderr, "open json output failed: %s, errno=%d\n", json_path, errno);
        return -1;
    }

    gettimeofday(&tv, TD_NULL);
    fprintf(fp, "{\n");
    fprintf(fp, "  \"ts_ms\": %lld,\n", (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000);
    fprintf(fp, "  \"device_id\": \"%s\",\n", device_id);
    fprintf(fp, "  \"camera_id\": %d,\n", camera_id);
    fprintf(fp, "  \"image_path\": \"%s\",\n", ppm_path);
    fprintf(fp, "  \"top1_id\": %d,\n", topk[0].class_id);
    fprintf(fp, "  \"top1_label\": \"%s\",\n", top1_name);
    fprintf(fp, "  \"top1_prob\": %.6f,\n", topk[0].prob);
    fprintf(fp, "  \"need_alert\": %s,\n", need_alert ? "true" : "false");
    fprintf(fp, "  \"consecutive_hits\": %d,\n", consecutive_hits);
    fprintf(fp, "  \"temperature_c\": %s,\n", g_sht3x_temp_buf);
    fprintf(fp, "  \"humidity_rh\": %s,\n", g_sht3x_rh_buf);
    fprintf(fp, "  \"topk\": [\n");
    for (k = 0; k < HESHI_TOPK; k++) {
        const char *name;
        int idx = topk[k].class_id;
        if (idx < 0) {
            continue;
        }
        name = label_name(labels, label_count, (size_t)idx, fallback, sizeof(fallback));
        fprintf(fp,
            "    {\"rank\": %d, \"id\": %d, \"logit\": %.8f, \"prob\": %.6f, \"label\": \"%s\"}%s\n",
            k, idx, topk[k].logit, topk[k].prob, name, (k == HESHI_TOPK - 1) ? "" : ",");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");
    fclose(fp);

    printf("json result saved: %s\n", json_path);
    return 0;
}

/*
 * 单阶段推理主流程。
 *
 * 这里完整覆盖了：
 * 抓帧 -> 预处理 -> ACL 推理 -> softmax/top-k -> 连续命中告警判定 -> 传感器读取 -> JSON 落盘。
 *
 * 注意：告警判定不是看单帧结果，而是看同一类别是否连续命中达到阈值，
 * 这样可以降低瞬时误检带来的误报。
 */
static int heshi_infer_one_round(
    uint32_t model_id,
    aclmdlDesc *desc,
    const char *json_path,
    const char *ppm_path,
    int camera_id,
    const char *device_id,
    int consecutive_n,
    float alert_threshold,
    int *consecutive_hits_io,
    int *last_alert_class_id_io,
    float center_crop_ratio,
    char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN],
    size_t label_count,
    heshi_result_summary_t *summary_out)
{
    /* input_dims:      模型输入维度描述，通常是 NCHW。
     * input/output:    ACL 输入输出数据集包装器。
     * rgb_image:       当前帧转换后的 RGB 图。
     * host_input:      CPU 侧预处理结果，稍后拷到 device memory。
     * host_output:     从 device 拷回来的原始 logits。
     * prob:            logits 经过 softmax 后的概率数组。
     * topk:            当前这一轮的前 5 类结果。
     * summary:         返回给上层主循环的摘要结论。
     * consecutive_hits_io / last_alert_class_id_io:
     *                  这是“跨轮次”的状态，不是本轮局部变量。
     *                  它们由调用方持有，在每轮推理间持续累积。
     */
    aclError ret;
    aclmdlIODims input_dims;
    ot_vpss_grp capture_grp = 0;
    heshi_dataset_t input = {0};
    heshi_dataset_t output = {0};
    heshi_rgb_image_t rgb_image = {0};
    ot_video_frame_info frame_info;
    uint8_t *host_input = NULL;
    size_t host_input_size = 0;
    float *host_output = NULL;
    float *prob = NULL;
    heshi_topk_item_t topk[HESHI_TOPK];
    heshi_result_summary_t summary = {0};
    size_t output_count;
    char fallback[32];

    printf("\n--- dual infer round ---\n");
    printf("camera: %d (VPSS grp %d)\n", camera_id, capture_grp);

    /* 1. 先从 VPSS 拿到当前现场画面。 */
    ret = heshi_capture_frame_rgb(capture_grp, DEFAULT_VPSS_CHN, ppm_path, &frame_info, &rgb_image);
    if (ret != TD_SUCCESS) {
        return -1;
    }
    if (g_stop_flag == 1) {
        goto free_rgb;
    }

    /* 2. 从模型描述里读取输入尺寸，避免把输入大小写死。 */
    memset(&input_dims, 0, sizeof(input_dims));
    ret = aclmdlGetInputDims(desc, 0, &input_dims);
    if (ret != ACL_ERROR_NONE || input_dims.dimCount < 4) {
        fprintf(stderr, "aclmdlGetInputDims failed, ret=%d\n", ret);
        goto free_rgb;
    }

    /* 3. 把 RGB 图做成 NCHW float32 输入。 */
    ret = preprocess_rgb_to_nchw(&rgb_image, (int)input_dims.dims[input_dims.dimCount - 1],
        (int)input_dims.dims[input_dims.dimCount - 2], center_crop_ratio, &host_input, &host_input_size);
    if (ret != 0) {
        goto free_rgb;
    }
    if (g_stop_flag == 1) {
        goto free_host_input;
    }

    /* 4. 输入拷贝到 device memory。 */
    ret = prepare_input_dataset(desc, host_input, host_input_size, &input);
    if (ret != 0) {
        goto free_host_input;
    }
    if (g_stop_flag == 1) {
        goto destroy_input;
    }

    /* 5. 为模型输出申请 device buffer。 */
    ret = prepare_output_dataset(desc, &output);
    if (ret != 0) {
        goto destroy_input;
    }
    if (g_stop_flag == 1) {
        goto destroy_output;
    }

    /* 6. 执行一次 ACL 模型。 */
    ret = aclmdlExecute(model_id, input.dataset, output.dataset);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclmdlExecute failed, ret=%d\n", ret);
        goto destroy_output;
    }
    printf("aclmdlExecute success\n");
    if (g_stop_flag == 1) {
        goto destroy_output;
    }

    /* 7. 把输出拷回主机侧，并解析出 top-k。 */
    /* output.size 是字节数，需要除以 sizeof(float) 才是类别数量。 */
    output_count = output.size / sizeof(float);
    host_output = (float *)malloc(output.size);
    prob = (float *)malloc(output.size);
    if (host_output == TD_NULL || prob == TD_NULL) {
        fprintf(stderr, "malloc output buffers failed\n");
        goto destroy_output;
    }

    ret = aclrtMemcpy(host_output, output.size, output.data, output.size, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtMemcpy output D2H failed, ret=%d\n", ret);
        goto free_output_host;
    }

    softmax(host_output, prob, output_count);
    compute_topk(host_output, output_count, prob, topk);
    print_topk(topk, labels, label_count);

    /* 8. 连续命中逻辑：
     * 只有“非健康类 + 概率足够高 + 连续多轮一致”才会标记 need_alert。
     */
    /* 先把 top1 结果写进 summary，供调用方后续决定是否发告警。 */
    summary.top1_id = topk[0].class_id;
    summary.top1_prob = topk[0].prob;
    if (topk[0].class_id >= 0) {
        const char *top1_name = label_name(labels, label_count, (size_t)topk[0].class_id, fallback, sizeof(fallback));
        /* 只有“不是健康类”并且“概率过阈值”时，才允许累计连续命中次数。 */
        if (heshi_is_healthy_like(top1_name) == TD_FALSE && topk[0].prob >= alert_threshold) {
            if (*last_alert_class_id_io == topk[0].class_id) {
                /* 同一类再次命中：连续计数加 1。 */
                (*consecutive_hits_io)++;
            } else {
                /* 类别发生变化：重置为“新类别的第一次命中”。 */
                *last_alert_class_id_io = topk[0].class_id;
                *consecutive_hits_io = 1;
            }
        } else {
            /* 健康类 / 低置信度结果不会参与连续命中统计。 */
            *last_alert_class_id_io = -1;
            *consecutive_hits_io = 0;
        }
    } else {
        *last_alert_class_id_io = -1;
        *consecutive_hits_io = 0;
    }
    /* 真正的业务告警条件：连续命中次数达到阈值。 */
    summary.need_alert = (*consecutive_hits_io >= consecutive_n) ? TD_TRUE : TD_FALSE;

    /* 9. 补充读取一次环境温湿度。 */
    heshi_read_sht3x();

    /* 10. 把本轮结果落到 JSON 文件。 */
    write_result_json(json_path, device_id, ppm_path, camera_id, summary.need_alert,
        *consecutive_hits_io, topk, labels, label_count);

    free(prob);
    free(host_output);
    destroy_dataset(&output);
    destroy_dataset(&input);
    free(host_input);
    free(rgb_image.rgb);
    if (summary_out) *summary_out = summary;
    return 0;

free_output_host:
    /* 下面这些 goto 出口按“谁申请谁释放”的顺序回收资源。 */
    if (prob != TD_NULL) {
        free(prob);
    }
    if (host_output != TD_NULL) {
        free(host_output);
    }
destroy_output:
    destroy_dataset(&output);
destroy_input:
    destroy_dataset(&input);
free_host_input:
    if (host_input != TD_NULL) {
        free(host_input);
    }
free_rgb:
    if (rgb_image.rgb != TD_NULL) {
        free(rgb_image.rgb);
    }
    return (g_stop_flag == 1) ? 0 : -1;
}

/* 把植物分类结果映射到对应病害模型编号。
 * Unknown / 背景类直接跳过第二阶段。
 */
static int heshi_plant_to_stage2(const char *plant_name)
{
    int i;
    if (strstr(plant_name, "Unknown") != NULL) return -1;
    for (i = 0; i < HESHI_STAGE2_MODELS; i++) {
        if (strncmp(plant_name, g_stage2_defs[i].plant_name, strlen(g_stage2_defs[i].plant_name)) == 0) {
            return i;
        }
    }
    return -1; /* fallback: skip disease stage */
}

/* 运行任意一个分类模型，供两阶段流程复用。 */
static int heshi_run_model(uint32_t model_id, aclmdlDesc *desc,
    uint8_t *host_input, size_t host_input_size,
    float *logits_out, size_t logit_count)
{
    /* 这是两阶段流程里的“小执行器”：
     * 调用它时，外部已经把图像预处理好了。
     * 它只负责：建输入 -> 建输出 -> 执行 ACL -> 拷回 logits。
     */
    aclError ret;
    heshi_dataset_t input = {0};
    heshi_dataset_t output = {0};
    float *host_output = NULL;
    size_t output_count;

    /* Create input dataset from preprocessed buffer */
    ret = prepare_input_dataset(desc, host_input, host_input_size, &input);
    if (ret != 0) return -1;

    /* Prepare output */
    ret = prepare_output_dataset(desc, &output);
    if (ret != 0) { destroy_dataset(&input); return -1; }

    /* Run inference */
    ret = aclmdlExecute(model_id, input.dataset, output.dataset);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclmdlExecute failed, ret=%d\n", ret);
        destroy_dataset(&output);
        destroy_dataset(&input);
        return -1;
    }

    /* Copy output */
    output_count = output.size / sizeof(float);
    host_output = (float *)malloc(output.size);
    if (host_output == NULL) {
        destroy_dataset(&output);
        destroy_dataset(&input);
        return -1;
    }

    ret = aclrtMemcpy(host_output, output.size, output.data, output.size, ACL_MEMCPY_DEVICE_TO_HOST);
    if (ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtMemcpy D2H failed, ret=%d\n", ret);
        free(host_output);
        destroy_dataset(&output);
        destroy_dataset(&input);
        return -1;
    }

    /* 为安全起见，只拷贝调用方实际准备好的 logits_out 容量。 */
    memcpy(logits_out, host_output, (output_count < logit_count ? output_count : logit_count) * sizeof(float));

    free(host_output);
    destroy_dataset(&output);
    destroy_dataset(&input);
    return 0;
}

/* 两阶段推理：
 * 先识别作物，再按作物惰性加载病害模型继续分类。
 * 预处理只做一次，同一份 NCHW 缓冲会复用给两个模型。
 */
static int heshi_infer_two_stage(
    heshi_model_slot_t *plant_slot,
    heshi_model_slot_t *disease_slots, size_t num_disease,
    const char *json_path, const char *ppm_path,
    int camera_id, const char *device_id,
    float center_crop_ratio,
    heshi_result_summary_t *summary_out)
{
    /* 这一轮两阶段推理里最关键的几个变量：
     * plant_name:     Stage1 判断出的作物名称。
     * stage2_idx:     该作物对应哪一个病害模型；-1 表示无需第二阶段。
     * combined_label: 最终拼出的 "Plant___Disease" 标签。
     * logits/prob:    Stage1 的输出缓存。
     * d_logits/d_prob:Stage2 的输出缓存（在局部 if 块里定义）。
     */
    aclmdlIODims input_dims;
    ot_vpss_grp capture_grp = 0;
    heshi_rgb_image_t rgb_image = {0};
    ot_video_frame_info frame_info;
    uint8_t *host_input = NULL;
    size_t host_input_size = 0;
    float *logits = NULL;
    float *prob = NULL;
    heshi_topk_item_t topk[HESHI_TOPK];
    heshi_result_summary_t summary = {0};
    size_t logit_count;
    char fallback[64];
    char combined_label[192];
    const char *plant_name;
    int stage2_idx;
    int ret;

    printf("\n--- two-stage infer round ---\n");
    printf("camera: %d (VPSS grp %d)\n", camera_id, capture_grp);

    /* 1. 抓取当前帧。 */
    ret = heshi_capture_frame_rgb(capture_grp, DEFAULT_VPSS_CHN, ppm_path, &frame_info, &rgb_image);
    if (ret != TD_SUCCESS) return -1;
    if (g_stop_flag == 1) goto free_rgb;

    /* 2. 预处理只做一次，供两个阶段共享。 */
    ret = aclmdlGetInputDims(plant_slot->desc, 0, &input_dims);
    if (ret != ACL_ERROR_NONE) { fprintf(stderr, "get input dims failed\n"); goto free_rgb; }

    ret = preprocess_rgb_to_nchw(&rgb_image, (int)input_dims.dims[input_dims.dimCount - 1],
        (int)input_dims.dims[input_dims.dimCount - 2], center_crop_ratio, &host_input, &host_input_size);
    if (ret != 0) goto free_rgb;
    if (g_stop_flag == 1) goto free_host_input;

    /* 3. 先跑植物分类，决定后续该走哪一个病害模型。 */
    logit_count = plant_slot->label_count;
    logits = (float *)malloc(logit_count * sizeof(float));
    prob = (float *)malloc(logit_count * sizeof(float));
    if (logits == NULL || prob == NULL) goto free_host_input;

    ret = heshi_run_model(plant_slot->model_id, plant_slot->desc,
        host_input, host_input_size, logits, logit_count);
    if (ret != 0) { fprintf(stderr, "plant model inference failed\n"); goto free_logits; }

    softmax(logits, prob, logit_count);
    compute_topk(logits, logit_count, prob, topk);
    printf("plant stage: ");
    print_topk(topk, plant_slot->labels, plant_slot->label_count);

    plant_name = label_name(plant_slot->labels, plant_slot->label_count,
        (size_t)topk[0].class_id, fallback, sizeof(fallback));
    printf("plant -> %s\n", plant_name);

    /* 4. 惰性加载对应病害模型，用完立即卸载，节省常驻内存。 */
    /* 根据植物类别决定后续该加载哪个病害模型。 */
    stage2_idx = heshi_plant_to_stage2(plant_name);
    if (stage2_idx >= 0 && (size_t)stage2_idx < num_disease) {
        heshi_model_slot_t *disease = &disease_slots[stage2_idx];
        size_t d_logit_count = disease->label_count;
        float *d_logits = (float *)malloc(d_logit_count * sizeof(float));
        float *d_prob = (float *)malloc(d_logit_count * sizeof(float));
        uint32_t disease_model_id = 0;
        aclmdlDesc *disease_desc = NULL;

        if (d_logits == NULL || d_prob == NULL) {
            /* 如果第二阶段缓存申请失败，就退化为只输出植物类别。 */
            free(d_logits);
            free(d_prob);
            snprintf(combined_label, sizeof(combined_label), "%s", plant_name);
            summary.top1_prob = topk[0].prob;
            goto write_json;
        }

        /* 按植物类别动态加载对应 OM 文件。 */
        {
            aclError load_ret = aclmdlLoadFromFile(g_stage2_defs[stage2_idx].model_path, &disease_model_id);
            if (load_ret == ACL_ERROR_NONE) {
                disease_desc = aclmdlCreateDesc();
                if (disease_desc != NULL) {
                    load_ret = aclmdlGetDesc(disease_desc, disease_model_id);
                }
            }
            if (load_ret != ACL_ERROR_NONE) {
                fprintf(stderr, "load disease model %s failed, ret=%d\n",
                    g_stage2_defs[stage2_idx].plant_name, load_ret);
            }
        }

        /* 加载成功后再继续病害分类。 */
        if (disease_model_id != 0 && disease_desc != NULL) {
            ret = heshi_run_model(disease_model_id, disease_desc,
                host_input, host_input_size, d_logits, d_logit_count);
            if (ret == 0) {
                softmax(d_logits, d_prob, d_logit_count);
                compute_topk(d_logits, d_logit_count, d_prob, topk);
                printf("disease stage (%s): ", g_stage2_defs[stage2_idx].plant_name);
                print_topk(topk, disease->labels, disease->label_count);

                const char *disease_name = label_name(disease->labels, disease->label_count,
                    (size_t)topk[0].class_id, fallback, sizeof(fallback));
                /* 两阶段最终对外统一仍然输出单个组合标签。 */
                snprintf(combined_label, sizeof(combined_label), "%s___%s", plant_name, disease_name);
                printf("combined -> %s\n", combined_label);
                summary.top1_prob = topk[0].prob;
            } else {
                fprintf(stderr, "disease model inference failed\n");
                snprintf(combined_label, sizeof(combined_label), "%s___Unknown", plant_name);
                summary.top1_prob = 0.0f;
            }
        } else {
            /* 没成功加载病害模型时，也至少保留植物分类结果。 */
            snprintf(combined_label, sizeof(combined_label), "%s", plant_name);
            summary.top1_prob = topk[0].prob;
        }

        /* 用完立即释放病害模型相关资源。 */
        if (disease_desc != NULL) aclmdlDestroyDesc(disease_desc);
        if (disease_model_id != 0) aclmdlUnload(disease_model_id);
        free(d_prob);
        free(d_logits);
    } else {
        /* Unknown plant — skip disease stage */
        snprintf(combined_label, sizeof(combined_label), "%s", plant_name);
        summary.top1_prob = topk[0].prob;
    }

write_json:
    /* 把两阶段组合标签映射回旧 16 类 ID，兼容现有云端接口和小程序。 */
    {
        static const char *k16[] = {
            "Apple___Apple_scab", "Apple___healthy",
            "Corn___Cercospora_leaf_spot Gray_leaf_spot", "Corn___Common_rust",
            "Corn___Northern_Leaf_Blight", "Corn___healthy",
            "Grape___Black_rot", "Grape___healthy",
            "Potato___Early_blight", "Potato___Late_blight", "Potato___healthy",
            "Tomato___Early_blight", "Tomato___Late_blight", "Tomato___Leaf_Mold",
            "Tomato___healthy", "Unknown_or_Background"
        };
        int k;
        /* 如果组合标签没命中旧 16 类中的任何一个，就保持 -1。 */
        summary.top1_id = -1;
        for (k = 0; k < 16; k++) {
            if (strcmp(combined_label, k16[k]) == 0) { summary.top1_id = k; break; }
        }
    }

    /* 5. 生成兼容旧格式的 JSON 输出。 */
    {
        heshi_topk_item_t combined_topk[HESHI_TOPK];
        char combined_labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN];
        size_t combined_count;
        memset(combined_topk, 0, sizeof(combined_topk));
        memset(combined_labels, 0, sizeof(combined_labels));

        /* 第一项固定是“作物___病害”的组合标签。 */
        strncpy(combined_labels[0], combined_label, HESHI_LABEL_LEN - 1);
        combined_topk[0].class_id = 0;
        combined_topk[0].prob = summary.top1_prob;
        /* 组合标签本身不是某个真实模型直接输出的 logit，所以这里填 0。 */
        combined_topk[0].logit = 0.0f;
        combined_count = 1;

        /* 后续项补充病害 top-k，便于调试第二阶段输出。 */
        {
            int k;
            for (k = 0; k < HESHI_TOPK && combined_count < HESHI_TOPK; k++) {
                char label_buf[HESHI_LABEL_LEN];
                const char *disease_name;
                if (stage2_idx >= 0 && (size_t)stage2_idx < num_disease) {
                    heshi_model_slot_t *disease = &disease_slots[stage2_idx];
                    disease_name = label_name(disease->labels, disease->label_count,
                        (size_t)topk[k].class_id, label_buf, sizeof(label_buf));
                } else {
                    disease_name = plant_name;
                }
                /* 这里理论上可能和第一项组合标签重复，当前实现保留简单兼容逻辑。 */
                if (k == 0 && strcmp(disease_name, "Late_blight") == 0) {
                    /* already covered by combined label, skip or use for extra info */
                }
                snprintf(combined_labels[combined_count], HESHI_LABEL_LEN - 1,
                    "%s___%s", plant_name, disease_name);
                combined_topk[combined_count].class_id = (int)combined_count;
                combined_topk[combined_count].prob = topk[k].prob;
                combined_topk[combined_count].logit = topk[k].logit;
                combined_count++;
            }
        }

        heshi_read_sht3x();
        write_result_json(json_path, device_id, ppm_path, camera_id, TD_FALSE,
            0, combined_topk, combined_labels, combined_count);
    }

    free(prob);
    free(logits);
    free(host_input);
    free(rgb_image.rgb);
    /* 把最终组合标签写回 summary，供主循环做连续命中判断。 */
    strncpy(summary.top1_label, combined_label, HESHI_LABEL_LEN - 1);
    if (summary_out) *summary_out = summary;
    return 0;

free_logits:
    free(prob);
    free(logits);
free_host_input:
    free(host_input);
free_rgb:
    free(rgb_image.rgb);
    return -1;
}

int main(int argc, char *argv[])
{
    /* positional:      收集位置参数，后面再按顺序解释含义。
     * model/labels/...:运行时实际生效的路径与参数，先给默认值。
     * consecutive_hits: 连续命中次数，跨轮次保留。
     * last_alert_class_id:
     *                  上一轮参与连续统计的类别 ID，配合 consecutive_hits 使用。
     * summary:         当前轮推理摘要，由 infer 函数回填。
     * plant_slot / disease_slots:
     *                  两阶段模式下保存模型句柄、描述和标签。
     */
    const char *positional[9];
    const char *model_path = DEFAULT_MODEL_PATH;
    const char *labels_path = DEFAULT_LABELS_PATH;
    const char *json_path = DEFAULT_JSON_PATH;
    const char *ppm_path = DEFAULT_PPM_PATH;
    const char *device_id = DEFAULT_DEVICE_ID;
    int camera_id = DEFAULT_CAMERA_ID;
    int loop_count = 0;
    int interval_sec = DEFAULT_INTERVAL_SEC;
    int consecutive_hits = 0;
    int last_alert_class_id = -1;
    int ret;
    int round = 0;
    heshi_result_summary_t summary = {0};
    alert_config_t alert_config;
    const int consecutive_n = DEFAULT_CONSECUTIVE_N;
    const float alert_threshold = DEFAULT_ALERT_THRESHOLD;
    float center_crop_ratio = DEFAULT_CENTER_CROP_RATIO;
    td_bool attach_only = TD_FALSE;
    td_bool reload_media = TD_FALSE;
    td_bool two_stage = TD_FALSE;
    heshi_model_slot_t plant_slot;
    heshi_model_slot_t disease_slots[HESHI_STAGE2_MODELS];
    heshi_camera_pipeline_t camera_pipeline;
    aclrtRunMode run_mode;
    aclError acl_ret;
    uint32_t model_id = 0;
    aclmdlDesc *desc = NULL;
    size_t label_count = 0;
    char labels[HESHI_MAX_LABELS][HESHI_LABEL_LEN] = {{0}};
    int i;
    int positional_count = 0;

    /* 开启行缓冲，保证日志在串口/重定向场景下也能及时刷出。 */
    setlinebuf(stdout);
    signal(SIGINT, heshi_signal_handler);
    signal(SIGTERM, heshi_signal_handler);

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    /* 解析命令行参数。 */
    memset(positional, 0, sizeof(positional));
    for (i = 1; i < argc; i++) {
        /* 先识别开关参数，再把剩余内容按“位置参数”收集起来。 */
        if (strcmp(argv[i], "--attach-only") == 0) {
            attach_only = TD_TRUE;
        } else if (strcmp(argv[i], "--reload-media") == 0) {
            reload_media = TD_TRUE;
        } else if (strcmp(argv[i], "--two-stage") == 0) {
            two_stage = TD_TRUE;
        } else if (positional_count < (int)(sizeof(positional) / sizeof(positional[0]))) {
            positional[positional_count++] = argv[i];
        } else {
            fprintf(stderr, "too many positional args: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    if (positional_count > 0) {
        model_path = positional[0];
    }
    if (positional_count > 1) {
        labels_path = positional[1];
    }
    if (positional_count > 2) {
        json_path = positional[2];
    }
    if (positional_count > 3) {
        ppm_path = positional[3];
    }
    if (positional_count > 4) {
        camera_id = atoi(positional[4]);
    }
    if (positional_count > 5) {
        device_id = positional[5];
    }
    if (positional_count > 6) {
        loop_count = atoi(positional[6]);
    }
    if (positional_count > 7) {
        interval_sec = atoi(positional[7]);
    }
    if (positional_count > 8) {
        center_crop_ratio = (float)atof(positional[8]);
    }

    /* 对用户输入做最基本的参数纠正，避免出现无意义或危险值。 */
    if (interval_sec < 1) {
        interval_sec = 1;
    }
    if (center_crop_ratio <= 0.0f || center_crop_ratio > 1.0f) {
        center_crop_ratio = DEFAULT_CENTER_CROP_RATIO;
    }

    (td_void)memset_s(&camera_pipeline, sizeof(camera_pipeline), 0, sizeof(camera_pipeline));

    /* 先把关键运行参数打印出来，方便串口日志里定位现场配置。 */
    printf("=== heshi_v2_dual_infer ===\n");
    printf("model   : %s\n", model_path);
    printf("labels  : %s\n", labels_path);
    printf("json    : %s\n", json_path);
    printf("ppm     : %s\n", ppm_path);
    printf("camera  : %d\n", camera_id);
    printf("interval: %d sec\n", interval_sec);
    printf("crop    : %.2f\n", center_crop_ratio);
    printf("mode    : %s\n", attach_only ? "attach-only" : "built-in single camera pipeline");

    /* 初始化告警分发与设备状态上报模块。 */
    alert_config_load(DEFAULT_ALERT_CONFIG_PATH, &alert_config);
    alert_dispatch_init(&alert_config);
    status_report_init(alert_config.device_id,
        alert_config.cloud_api_url, alert_config.cloud_api_key);

    /* 初始化温湿度传感器；失败时允许系统继续运行。 */
    if (sht3x_open("/dev/i2c-0", 0x44) == 0) {
        printf("sht3x: sensor opened on /dev/i2c-0\n");
    } else {
        printf("sht3x: not available (continuing without sensor)\n");
    }

    /* 可选：先恢复媒体栈，适合异常后自愈重启。 */
    if (reload_media == TD_TRUE) {
        if (attach_only == TD_TRUE) {
            fprintf(stderr, "--reload-media and --attach-only are mutually exclusive\n");
            return -1;
        }
        if (heshi_reset_media_stack() != 0) {
            fprintf(stderr, "media reload failed; check %s\n", DEFAULT_LOAD_LOG);
            return -1;
        }
        printf("media stack reloaded successfully\n");
    }

    /* 启动自建媒体管线，或者接管外部已运行的 VPSS。 */
    if (attach_only == TD_FALSE) {
        ret = heshi_start_camera_pipeline(&camera_pipeline);
        if (ret != TD_SUCCESS) {
            fprintf(stderr, "built-in camera pipeline start failed\n");
            heshi_stop_camera_pipeline(&camera_pipeline);
            return -1;
        }
        /* 让摄像头先稳定一下，避免首帧曝光异常。 */
        sleep(1);
    } else {
        ret = heshi_prepare_attach_vpss(0, DEFAULT_VPSS_CHN);
        if (ret != TD_SUCCESS) {
            fprintf(stderr, "attach vpss failed; is the VPSS pipeline running?\n");
            return -1;
        }
    }

    /* 标签文件只读取一次并常驻内存。 */
    label_count = load_labels(labels_path, labels);
    printf("loaded labels: %zu\n", label_count);

    /* ACL / NPU 资源在整个进程生命周期内只初始化一次。 */
    acl_ret = aclInit("");
    if (acl_ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclInit failed, ret=%d\n", acl_ret);
        goto stop_pipeline;
    }

    acl_ret = aclrtSetDevice(HESHI_DEVICE_ID);
    if (acl_ret != ACL_ERROR_NONE) {
        fprintf(stderr, "aclrtSetDevice failed, ret=%d\n", acl_ret);
        goto finalize_acl;
    }

    acl_ret = aclrtGetRunMode(&run_mode);
    if (acl_ret != ACL_ERROR_NONE || run_mode != ACL_DEVICE) {
        fprintf(stderr, "aclrtGetRunMode failed or not ACL_DEVICE, ret=%d mode=%d\n", acl_ret, run_mode);
        goto reset_device;
    }

    /* 根据 two_stage 开关，走两种完全不同的模型组织方式：
     * 1. two_stage=true:  先植物模型，再按需加载病害模型。
     * 2. two_stage=false: 直接跑旧的单阶段 16 类模型。
     */
    if (two_stage) {
        /* --- Two-stage with lazy disease model loading --- */
        int si;

        /* 先清零，确保后续任意一步失败时释放逻辑都能安全判断。 */
        memset(&plant_slot, 0, sizeof(plant_slot));
        memset(disease_slots, 0, sizeof(disease_slots));

        /* 植物模型是两阶段流程的入口，所以必须先常驻加载。 */
        acl_ret = aclmdlLoadFromFile(HESHI_PLANT_MODEL, &plant_slot.model_id);
        if (acl_ret != ACL_ERROR_NONE) { fprintf(stderr, "load plant model failed\n"); goto reset_device; }
        plant_slot.desc = aclmdlCreateDesc();
        if (plant_slot.desc == NULL) goto unload_models;
        acl_ret = aclmdlGetDesc(plant_slot.desc, plant_slot.model_id);
        if (acl_ret != ACL_ERROR_NONE) goto unload_models;
        plant_slot.label_count = load_labels(HESHI_PLANT_LABELS, plant_slot.labels);
        printf("plant model loaded, id=%u labels=%zu\n", plant_slot.model_id, plant_slot.label_count);

        /* 病害模型本体不预加载，但标签文件很小，可以先一次性读入。 */
        for (si = 0; si < HESHI_STAGE2_MODELS; si++) {
            disease_slots[si].stage2_index = si;
            disease_slots[si].label_count = load_labels(g_stage2_defs[si].labels_path, disease_slots[si].labels);
            printf("disease[%s] labels: %zu\n", g_stage2_defs[si].plant_name, disease_slots[si].label_count);
        }

        /* 两阶段主循环：
         * 每一轮内部会：
         * 1. 抓图并预处理。
         * 2. 跑植物分类。
         * 3. 按植物结果临时加载病害模型。
         * 4. 组合成旧 16 类兼容标签。
         * 5. 回到这里做连续命中判断和告警发送。
         */
        last_alert_class_id = -1;
        consecutive_hits = 0;
        /* 主循环：
         * 每轮都先做两阶段推理，再基于组合标签结果做连续命中告警判定。
         */
        do {
            if (g_stop_flag == 1) break;
            round++;
            ret = heshi_infer_two_stage(&plant_slot, disease_slots, HESHI_STAGE2_MODELS,
                json_path, ppm_path, 0, device_id, center_crop_ratio, &summary);
            if (ret != 0) {
                /* 单轮失败时不中止整个进程，而是打印日志后继续下一轮。 */
                if (g_stop_flag == 1) break;
                fprintf(stderr, "round %d two-stage inference failed, continuing...\n", round);
                continue;
            }

            /* 两阶段模式下，连续命中统计放在主循环中完成，
             * 因为真正的“业务类别”是组合后的旧 16 类 ID。
             */
            {
                const char *label = summary.top1_label;
                td_bool is_healthy = heshi_is_healthy_like(label);
                if (is_healthy == TD_FALSE && summary.top1_prob >= alert_threshold) {
                    if (last_alert_class_id == summary.top1_id) {
                        /* 同一类连续再次出现，累计命中数。 */
                        consecutive_hits++;
                    } else {
                        /* 类别变化时，从 1 重新开始累计。 */
                        last_alert_class_id = summary.top1_id;
                        consecutive_hits = 1;
                    }
                } else {
                    /* 健康类或低置信度结果直接清空累计状态。 */
                    last_alert_class_id = -1;
                    consecutive_hits = 0;
                }
            }

            /* 只有达到连续阈值后，才真正把当前截图和结果发给告警模块。 */
            if (consecutive_hits >= consecutive_n && summary.top1_prob >= alert_threshold) {
                struct timeval tv;
                gettimeofday(&tv, TD_NULL);
                {
                    /* 告警时读取最近一次环境缓存，作为伴随信息上报。 */
                    float alert_t = -999, alert_h = -999;
                    sht3x_get_last(&alert_t, &alert_h);
                    alert_dispatch_send(ppm_path, 0,
                        summary.top1_id, summary.top1_label, summary.top1_prob,
                        consecutive_hits,
                        (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000,
                        alert_t, alert_h);
                }
            }
            /* 无论是否发告警，都按周期上报设备在线状态。 */
            status_report_tick();
            if (loop_count == 1) break;
            if (loop_count > 1 && round >= loop_count) break;
            if (g_stop_flag == 1) break;
            /* 两轮之间按 interval_sec 休眠，避免持续满速推理。 */
            heshi_msleep_interruptible(interval_sec * 1000);
        } while (1);

        printf("\nstopping after %d rounds\n", round);

        /* 两阶段模式退出时：
         * 植物模型是在主循环外常驻加载的，所以这里统一释放。
         * 病害模型则是在每轮内部按需加载、按需释放。
         */
        if (plant_slot.desc != NULL) aclmdlDestroyDesc(plant_slot.desc);
        aclmdlUnload(plant_slot.model_id);
        /* Disease models are loaded/unloaded per-round in heshi_infer_two_stage */
        (void)aclrtResetDevice(HESHI_DEVICE_ID);
        (void)aclFinalize();
    } else {
        /* --- Legacy single-model mode --- */
        /* 单阶段模式下只需要加载一个总模型，逻辑会简单很多。 */
        acl_ret = aclmdlLoadFromFile(model_path, &model_id);
        if (acl_ret != ACL_ERROR_NONE) {
            fprintf(stderr, "aclmdlLoadFromFile failed, ret=%d\n", acl_ret);
            goto single_reset_device;
        }
        printf("model loaded, id=%u\n", model_id);

        desc = aclmdlCreateDesc();
        if (desc == TD_NULL) {
            fprintf(stderr, "aclmdlCreateDesc failed\n");
            goto single_unload_model;
        }

        acl_ret = aclmdlGetDesc(desc, model_id);
        if (acl_ret != ACL_ERROR_NONE) {
            fprintf(stderr, "aclmdlGetDesc failed, ret=%d\n", acl_ret);
            goto single_destroy_desc;
        }

        /* 单阶段主循环：
         * 每一轮由 heshi_infer_one_round 内部完成抓图、推理、连续命中统计和 JSON 输出。
         */
        do {
            if (g_stop_flag == 1) break;
            round++;

            ret = heshi_infer_one_round(model_id, desc, json_path, ppm_path,
                0, device_id, consecutive_n, alert_threshold,
                &consecutive_hits, &last_alert_class_id, center_crop_ratio,
                labels, label_count, &summary);
            if (ret != 0) {
                /* 单轮失败时记录日志，但不立刻退出整个守护进程。 */
                if (g_stop_flag == 1) break;
                fprintf(stderr, "round %d inference failed, continuing...\n", round);
            }

            /* 单阶段模式下，是否满足告警条件已经在 infer_one_round 里算好。 */
            if (ret == 0 && summary.need_alert) {
                char fallback[32];
                const char *label = label_name(labels, label_count,
                    (size_t)summary.top1_id, fallback, sizeof(fallback));
                struct timeval tv;
                gettimeofday(&tv, TD_NULL);
                {
                    /* 告警图片路径仍然复用当前轮抓到的 ppm_path。 */
                    float alert_t = -999, alert_h = -999;
                    sht3x_get_last(&alert_t, &alert_h);
                    alert_dispatch_send(ppm_path, 0,
                        summary.top1_id, label, summary.top1_prob,
                        consecutive_hits,
                        (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000,
                        alert_t, alert_h);
                }
            }

            /* 定时心跳上报，让云端能判断设备是否在线。 */
            status_report_tick();

            if (loop_count == 1) break;
            if (loop_count > 1 && round >= loop_count) break;
            if (g_stop_flag == 1) break;
            /* 控制推理节奏，避免 CPU/NPU 与摄像头链路长时间满负载。 */
            heshi_msleep_interruptible(interval_sec * 1000);
        } while (1);

        printf("\nstopping after %d rounds (stop_flag=%d)\n", round, (int)g_stop_flag);

        /* 下面这一串标签式清理是典型 C 风格资源回收：
         * 从“最后可能成功申请的资源”开始，一级级往前释放。
         */
    single_destroy_desc:
        if (desc != TD_NULL) {
            (void)aclmdlDestroyDesc(desc);
        }
    single_unload_model:
        if (model_id != 0) {
            (void)aclmdlUnload(model_id);
        }
    single_reset_device:
        (void)aclrtResetDevice(HESHI_DEVICE_ID);
    single_finalize_acl:
        (void)aclFinalize();
    }  /* end legacy single-model */

    goto stop_pipeline;

unload_models:
    /* 两阶段模式在“模型尚未全部加载完成”时出错，会走到这里。
     * 由于前面已经把结构体清零，所以可以安全地逐项检查并释放。
     */
    {
        int ci;
        if (plant_slot.desc != NULL) aclmdlDestroyDesc(plant_slot.desc);
        if (plant_slot.model_id != 0) aclmdlUnload(plant_slot.model_id);
        for (ci = 0; ci < HESHI_STAGE2_MODELS; ci++) {
            if (disease_slots[ci].desc != NULL) aclmdlDestroyDesc(disease_slots[ci].desc);
            if (disease_slots[ci].model_id != 0) aclmdlUnload(disease_slots[ci].model_id);
        }
    }
reset_device:
    (void)aclrtResetDevice(HESHI_DEVICE_ID);
finalize_acl:
    (void)aclFinalize();

stop_pipeline:
    /* 最外层统一收尾：
     * 1. 停媒体管线
     * 2. 关传感器
     * 3. 关心跳模块
     * 4. 关告警模块
     */
    if (attach_only == TD_FALSE) {
        heshi_stop_camera_pipeline(&camera_pipeline);
    }

    sht3x_close();
    status_report_deinit();
    alert_dispatch_deinit();

    printf("exit.\n");
    /* 如果是人为发信号结束，返回 0；否则把最后一次错误码往外传。 */
    return (g_stop_flag == 1) ? 0 : ret;
}

# motion — IMU 采样任务 + 计步/距离/卡路里服务
#
# 为什么单独一层（而不是塞进 data_hub）：
#   data_hub 是"设备状态快照"，它不该知道 IMU 是怎么采样的。
#   motion 负责"50Hz 读加速度 → 跑计步算法 → 把结果推给 data_hub"，
#   data_hub 只管存和广播。以后换别的 IMU 或换算法，只动 motion。
idf_component_register(
    SRCS "motion.c"
    INCLUDE_DIRS "include"
    REQUIRES imu imu_logger data_hub settings
    PRIV_REQUIRES esp_timer event_bus
)

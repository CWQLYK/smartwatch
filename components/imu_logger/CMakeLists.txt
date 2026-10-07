# imu_logger — 把 IMU 原始数据录到 Flash，供离线调参
#
# 为什么要它：
#   调计步参数需要**真实走路**的 IMU 数据。但串口线很短，只能坐着甩手，
#   甩手和走路的信号特征完全不是一回事。所以让手表自己把数据录进 Flash，
#   走完回来插上 USB 再导出到电脑上离线跑算法。
#
# 录的是**原始加速度**（时间戳 + ax/ay/az，每帧 12 字节），
# 不录算法中间量 —— 原始数据可以反复用不同参数重算，不用重新走路。
idf_component_register(
    SRCS "imu_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs esp_partition
    PRIV_REQUIRES esp_timer
)

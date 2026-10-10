# pwr_log — 功耗日志（把电池电压录到 Flash，供离线算真实省电）
#
# 为什么要它：
#   想证明"这一版真的省电了"，正常要么有 USB 电流表、要么拔线比续航。
#   本项目两者都没有，而且有个硬约束：插着 USB 时 VBUS 直接给整机供电 + 给电池充电，
#   量到的是"充电器在干活"；不插 USB 又没有串口、没法一边待机一边看数据。
#   → 所以反过来：**让手表自己记**（电池 mV + 墙上时间），回来插上线一条命令导出。
#
# 依赖说明：
#   spiffs / esp_partition —— 使用分区 "storage"（挂载由 imu_logger 在开机时完成，
#                             本组件只使用、不挂载）
#   esp_timer（私有）     —— 每 30 秒采一次
#   data_hub（私有）      —— 取"已经采好且加锁"的电量快照，而不是自己调 battery_read()
#                             （那是为了避免在它已有的无锁共享缓冲上再加一个并发调用者）
idf_component_register(
    SRCS "pwr_log.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs esp_partition
    PRIV_REQUIRES esp_timer data_hub bsp
)

# imu — QMI8658C 六轴 IMU 驱动 + 腕戴计步/距离/卡路里
#
# 寄存器定义出处：QMI8658A Datasheet Rev A (QST, Document# 13-52-25)
#   Table 22 = CTRL1..CTRL9，Table 23 = STATUS0/1，
#   Table 25 = 数据寄存器 0x35..0x40，Table 27 = RESET(0x60)
#
# I2C 走 board 组件的共享 400kHz 总线（地址 0x6B）。
idf_component_register(
    SRCS "qmi8658c.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_timer
)

################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/board_id.c \
../Core/Src/can_test.c \
../Core/Src/console.c \
../Core/Src/fatfs_rtc.c \
../Core/Src/fatfs_test.c \
../Core/Src/hyperram_test.c \
../Core/Src/main.c \
../Core/Src/rtc_test.c \
../Core/Src/sd_test.c \
../Core/Src/stm32h7xx_hal_msp.c \
../Core/Src/stm32h7xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32h7xx.c 

OBJS += \
./Core/Src/board_id.o \
./Core/Src/can_test.o \
./Core/Src/console.o \
./Core/Src/fatfs_rtc.o \
./Core/Src/fatfs_test.o \
./Core/Src/hyperram_test.o \
./Core/Src/main.o \
./Core/Src/rtc_test.o \
./Core/Src/sd_test.o \
./Core/Src/stm32h7xx_hal_msp.o \
./Core/Src/stm32h7xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32h7xx.o 

C_DEPS += \
./Core/Src/board_id.d \
./Core/Src/can_test.d \
./Core/Src/console.d \
./Core/Src/fatfs_rtc.d \
./Core/Src/fatfs_test.d \
./Core/Src/hyperram_test.d \
./Core/Src/main.d \
./Core/Src/rtc_test.d \
./Core/Src/sd_test.d \
./Core/Src/stm32h7xx_hal_msp.d \
./Core/Src/stm32h7xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32h7xx.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DUSE_PWR_LDO_SUPPLY -DUSE_HAL_DRIVER -DSTM32H735xx -c -I../Core/Inc -I../Drivers/STM32H7xx_HAL_Driver/Inc -I../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../Drivers/CMSIS/Include -I../FATFS/Target -I../FATFS/App -I../Middlewares/Third_Party/FatFs/src -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/board_id.cyclo ./Core/Src/board_id.d ./Core/Src/board_id.o ./Core/Src/board_id.su ./Core/Src/can_test.cyclo ./Core/Src/can_test.d ./Core/Src/can_test.o ./Core/Src/can_test.su ./Core/Src/console.cyclo ./Core/Src/console.d ./Core/Src/console.o ./Core/Src/console.su ./Core/Src/fatfs_rtc.cyclo ./Core/Src/fatfs_rtc.d ./Core/Src/fatfs_rtc.o ./Core/Src/fatfs_rtc.su ./Core/Src/fatfs_test.cyclo ./Core/Src/fatfs_test.d ./Core/Src/fatfs_test.o ./Core/Src/fatfs_test.su ./Core/Src/hyperram_test.cyclo ./Core/Src/hyperram_test.d ./Core/Src/hyperram_test.o ./Core/Src/hyperram_test.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/rtc_test.cyclo ./Core/Src/rtc_test.d ./Core/Src/rtc_test.o ./Core/Src/rtc_test.su ./Core/Src/sd_test.cyclo ./Core/Src/sd_test.d ./Core/Src/sd_test.o ./Core/Src/sd_test.su ./Core/Src/stm32h7xx_hal_msp.cyclo ./Core/Src/stm32h7xx_hal_msp.d ./Core/Src/stm32h7xx_hal_msp.o ./Core/Src/stm32h7xx_hal_msp.su ./Core/Src/stm32h7xx_it.cyclo ./Core/Src/stm32h7xx_it.d ./Core/Src/stm32h7xx_it.o ./Core/Src/stm32h7xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32h7xx.cyclo ./Core/Src/system_stm32h7xx.d ./Core/Src/system_stm32h7xx.o ./Core/Src/system_stm32h7xx.su

.PHONY: clean-Core-2f-Src


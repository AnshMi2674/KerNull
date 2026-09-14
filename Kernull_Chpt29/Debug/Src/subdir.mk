################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Src/kernel.c \
../Src/list.c \
../Src/main.c \
../Src/mutex.c \
../Src/semaphore.c \
../Src/semaphore_test_priority.c \
../Src/semaphore_tests_basic.c \
../Src/syscalls.c \
../Src/sysmem.c 

OBJS += \
./Src/kernel.o \
./Src/list.o \
./Src/main.o \
./Src/mutex.o \
./Src/semaphore.o \
./Src/semaphore_test_priority.o \
./Src/semaphore_tests_basic.o \
./Src/syscalls.o \
./Src/sysmem.o 

C_DEPS += \
./Src/kernel.d \
./Src/list.d \
./Src/main.d \
./Src/mutex.d \
./Src/semaphore.d \
./Src/semaphore_test_priority.d \
./Src/semaphore_tests_basic.d \
./Src/syscalls.d \
./Src/sysmem.d 


# Each subdirectory must supply rules for building sources it contributes
Src/%.o Src/%.su Src/%.cyclo: ../Src/%.c Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DSTM32F103RBTx -DSTM32 -DSTM32F1 -c -I../Inc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-Src

clean-Src:
	-$(RM) ./Src/kernel.cyclo ./Src/kernel.d ./Src/kernel.o ./Src/kernel.su ./Src/list.cyclo ./Src/list.d ./Src/list.o ./Src/list.su ./Src/main.cyclo ./Src/main.d ./Src/main.o ./Src/main.su ./Src/mutex.cyclo ./Src/mutex.d ./Src/mutex.o ./Src/mutex.su ./Src/semaphore.cyclo ./Src/semaphore.d ./Src/semaphore.o ./Src/semaphore.su ./Src/semaphore_test_priority.cyclo ./Src/semaphore_test_priority.d ./Src/semaphore_test_priority.o ./Src/semaphore_test_priority.su ./Src/semaphore_tests_basic.cyclo ./Src/semaphore_tests_basic.d ./Src/semaphore_tests_basic.o ./Src/semaphore_tests_basic.su ./Src/syscalls.cyclo ./Src/syscalls.d ./Src/syscalls.o ./Src/syscalls.su ./Src/sysmem.cyclo ./Src/sysmem.d ./Src/sysmem.o ./Src/sysmem.su

.PHONY: clean-Src


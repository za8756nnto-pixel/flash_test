################################################################################
# Automatically-generated file. Do not edit!
################################################################################

SHELL = cmd.exe

# Each subdirectory must supply rules for building sources it contributes
device.obj: C:/ti/c2000/C2000Ware_6_00_01_00/device_support/f280015x/common/source/device.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: C2000 Compiler'
	"C:/ti/ccs1281/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/bin/cl2000" -v28 -ml -mt --float_support=fpu32 --tmu_support=tmu0 --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/libraries/flash_api/f280015x/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/libraries/flash_api/f280015x/include/FlashAPI" --include_path="C:/ushiwaka/TI_FLASH/Flash_Test/TEST_FLash_Code" --include_path="C:/ti/ccs1281/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/device_support/f280015x/common/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/driverlib/f280015x/driverlib" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/device_support/f280015x/headers/include" --advice:performance=all --define=_LAUNCHXL_F2800157 --define=_FLASH -g --diag_warning=225 --diag_wrap=off --display_error_number --abi=eabi --preproc_with_compile --preproc_dependency="$(basename $(<F)).d_raw" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '

%.obj: ../%.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: C2000 Compiler'
	"C:/ti/ccs1281/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/bin/cl2000" -v28 -ml -mt --float_support=fpu32 --tmu_support=tmu0 --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/libraries/flash_api/f280015x/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/libraries/flash_api/f280015x/include/FlashAPI" --include_path="C:/ushiwaka/TI_FLASH/Flash_Test/TEST_FLash_Code" --include_path="C:/ti/ccs1281/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/device_support/f280015x/common/include" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/driverlib/f280015x/driverlib" --include_path="C:/ti/c2000/C2000Ware_6_00_01_00/device_support/f280015x/headers/include" --advice:performance=all --define=_LAUNCHXL_F2800157 --define=_FLASH -g --diag_warning=225 --diag_wrap=off --display_error_number --abi=eabi --preproc_with_compile --preproc_dependency="$(basename $(<F)).d_raw" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '



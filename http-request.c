// SPDX-License-Identifier: BSD-2-Clause-Patent

/*
 * Copyright 2025 Wangwei 
 * Copyright 2025 ISCAS Zhangtieyi <zhangtieyi@iscas.ac.cn>
 */
#include "shim.h"
#include "http.h"

EFI_HTTP_METHOD http_request_method;
CHAR8 *tx_body_json = NULL;


EFI_STATUS
print_device_path(EFI_HANDLE image_handle,
                  EFI_HANDLE http_binding_handle)
{
	EFI_STATUS efi_status;
	EFI_DEVICE_PATH_PROTOCOL *nic_device_path_protocol = NULL;
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *device_path_to_text_protocol = NULL;
	CHAR16 *text_device_path = NULL;

	efi_status = BS->OpenProtocol(http_binding_handle, &DevicePathProtocol,
	                              (VOID **)&nic_device_path_protocol,
	                              image_handle, NULL,
	                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(efi_status)) {
		perror(L"Failed to open nic device path protocol: %r\n", efi_status);
		return efi_status;
	}

	efi_status = BS->LocateProtocol(&DevicePathToTextProtocol, NULL,
	                                (VOID **)&device_path_to_text_protocol);
	if (EFI_ERROR(efi_status) || device_path_to_text_protocol == NULL) {
		perror(L"Failed to locate device path to text protocol: %r\n", efi_status);
		return efi_status;
	}

	text_device_path =
		device_path_to_text_protocol->ConvertDevicePathToText(
			nic_device_path_protocol,
			FALSE, // DisplayOnly
			FALSE  // AllowShortcuts
		);

	if (text_device_path != NULL) {
		console_print(L"HTTP binding device path: %s\n", text_device_path);
		BS->FreePool(text_device_path);
		text_device_path = NULL;
	}

	return EFI_SUCCESS;
}

// EFI_STATUS
// print_device_path(EFI_HANDLE image_handle,
//                   EFI_HANDLE http_binding_handle)
// {
// 	EFI_STATUS efi_status;
// 	EFI_DEVICE_PATH_PROTOCOL *nic_device_path_protocol = NULL;
// 	efi_status = BS->OpenProtocol(http_binding_handle, &DevicePathProtocol,
// 	                              (void **)&nic_device_path_protocol,
// 	                              image_handle, NULL,
// 	                              EFI_OPEN_PROTOL_GET_PROTOCOL);
// 	if (EFI_ERROR(efi_status)) {
// 		perror(L"Failed to open nic device path protorol\n");
// 		return efi_status;
// 	}
// 	UINTN to_text_handle_count = 0;
// 	EFI_HANDLE *to_text_handles = NULL;
// 	efi_status =
// 		BS->LocateHandleBuffer(ByProtocol, &DevicePathToTextProtocol,
// 	                               NULL, &to_text_handle_count,
// 	                               &to_text_handles);
// 	if (EFI_ERROR(efi_status)) {
// 		perror(L"Failed to locate device path to text protorol\n");
// 		goto close_nic_protocol;
// 	}
// 	if (to_text_handle_count == 0) {
// 		efi_status = EFI_NOT_FOUND;
// 		goto free_handles;
// 	}
// 	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *device_path_to_text_protocol = NULL;
// 	efi_status =
// 		BS->OpenProtocol(to_text_handles[0], &DevicePathToTextProtocol,
// 	                         (void **)&device_path_to_text_protocol,
// 	                         image_handle, NULL,
// 	                         EFI_OPEN_PROTOCOL_GET_PROTOCOL);
// 	if (EFI_ERROR(efi_status)) {
// 		perror(L"Failed to open device path to text protorol\n");
// 		goto free_handles;
// 	}
// 	CHAR16 *text_device_path =
// 		device_path_to_text_protocol->ConvertDevicePathToText(
// 			nic_device_path_protocol,
// 			FALSE, // DisplayOnly: FALSE for full path, TRUE for abbreviated
// 			FALSE // AllowShortcuts: FALSE to disable shortcut expansion
// 		);
// 	if (text_device_path != NULL) {
// 		BS->FreePool(text_device_path);
// 	}
// 	BS->CloseProtocol(to_text_handles[0], &DevicePathToTextProtocol,
// 	                  image_handle, NULL);
// 
// free_handles:
// 	if (to_text_handles) {
// 		BS->FreePool(to_text_handles);
// 	}
// 
// close_nic_protocol:
// 	BS->CloseProtocol(http_binding_handle, &DevicePathProtocol,
// 	                  image_handle, NULL);
// 	return efi_status;
// }

// Remember to free output outside if it's not null.
EFI_STATUS
ip4_cfg2_get_data(EFI_IP4_CONFIG2_PROTOCOL *ip4_cfg2_protocol,
                  EFI_IP4_CONFIG2_DATA_TYPE data_type, void **output)
{
	EFI_STATUS efi_status;
	UINTN get_size = 0;
	efi_status = ip4_cfg2_protocol->GetData(ip4_cfg2_protocol, data_type,
	                                        &get_size, NULL);
	if (efi_status != EFI_BUFFER_TOO_SMALL) {
		perror(L"Failed to get ip4 config2 data size\n");
		return efi_status;
	}
	efi_status = BS->AllocatePool(EfiBootServicesData, get_size, output);
	if (EFI_ERROR(efi_status)) {
		perror(L"Failed to allocate memory for data\n");
		return efi_status;
	}
	efi_status = ip4_cfg2_protocol->GetData(ip4_cfg2_protocol, data_type,
	                                        &get_size, *output);
	if (EFI_ERROR(efi_status)) {
		perror(L"Failed to get ip4 config2 data size\n");
		return efi_status;
	}
	return EFI_SUCCESS;
}

BOOLEAN
check_ip4_addr(EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info)
{
	return ip4_cfg2_iface_info->StationAddress.Addr[0] +
	               ip4_cfg2_iface_info->StationAddress.Addr[1] +
	               ip4_cfg2_iface_info->StationAddress.Addr[2] +
	               ip4_cfg2_iface_info->StationAddress.Addr[3] !=
	       0;
}

void
print_ip4_addr_verbose(EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info)
{
	console_print(L"Ip4 addr=%d.%d.%d.%d\n",
	              ip4_cfg2_iface_info->StationAddress.Addr[0],
	              ip4_cfg2_iface_info->StationAddress.Addr[1],
	              ip4_cfg2_iface_info->StationAddress.Addr[2],
	              ip4_cfg2_iface_info->StationAddress.Addr[3]);
}

// Remember to free output outside if it's not null.
EFI_STATUS
wait_until_get_iface_info(EFI_IP4_CONFIG2_PROTOCOL *ip4_cfg2_protocol,
                          EFI_IP4_CONFIG2_INTERFACE_INFO **p_ip4_cfg2_iface_info)
{
	EFI_STATUS efi_status;
	assert(p_ip4_cfg2_iface_info);
	if (*p_ip4_cfg2_iface_info) {
		BS->FreePool(*p_ip4_cfg2_iface_info);
		*p_ip4_cfg2_iface_info = NULL;
	}
	EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info = NULL;
	for (int i = 0; i < 30; i++) {
		console_print(L".");
		usleep(1000000);
		efi_status = ip4_cfg2_get_data(ip4_cfg2_protocol,
		                               Ip4Config2DataTypeInterfaceInfo,
		                               (void **)&ip4_cfg2_iface_info);
		if (EFI_ERROR(efi_status)) {
			perror(L"Failed to open ip4 config2 protorol\n");
			return efi_status;
		}
		if (check_ip4_addr(ip4_cfg2_iface_info)) {
			print_ip4_addr_verbose(ip4_cfg2_iface_info);
			*p_ip4_cfg2_iface_info = ip4_cfg2_iface_info;
			break;
		}
		if (ip4_cfg2_iface_info) {
			BS->FreePool(ip4_cfg2_iface_info);
			ip4_cfg2_iface_info = NULL;
		}
	}
	if (*p_ip4_cfg2_iface_info) {
		return EFI_SUCCESS;
	} else {
		return EFI_NOT_READY;
	}
}


// =================================================================
// 1. GUID 定义 (仅包含标准头文件中没有的)
// =================================================================

// TCP Service Binding GUID (保留，gnu-efi 通常不包含)
static EFI_GUID gEfiTcp4ServiceBindingProtocolGuid = 
    { 0x00720665, 0x67EB, 0x4A99, { 0xBA, 0xF7, 0xD3, 0xC3, 0x2E, 0x2C, 0x12, 0x43 } };

// HTTP Service Binding GUID (保留，gnu-efi 通常不包含)
static EFI_GUID gEfiHttpServiceBindingProtocolGuid = 
    { 0xbdc8176e, 0x4bcd, 0x4033, { 0xbe, 0xa2, 0x43, 0xa3, 0x2c, 0x73, 0x53, 0x4c } };

// 宏定义
#define EFI_TCP_BINDING_GUID  gEfiTcp4ServiceBindingProtocolGuid
#define EFI_HTTP_BINDING_GUID gEfiHttpServiceBindingProtocolGuid

// 注意：gEfiLoadedImageProtocolGuid, gEfiSimpleFileSystemProtocolGuid, gEfiFileInfoGuid
// 已经在 gnu-efi 头文件中定义，无需在此重复定义，直接使用即可。

// =================================================================
// 2. 从文件加载驱动的函数
// =================================================================

EFI_STATUS LoadDriverFromFile(EFI_HANDLE ImageHandle, CHAR16 *FileName)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem = NULL;
    EFI_FILE_PROTOCOL *RootDir = NULL;
    EFI_FILE_PROTOCOL *FileHandle = NULL;
    EFI_FILE_INFO *FileInfo = NULL;
    UINTN FileInfoSize = sizeof(EFI_FILE_INFO) + 512;
    VOID *FileBuffer = NULL;
    UINTN FileSize;
    EFI_HANDLE DriverHandle = NULL;

    // 1. 获取当前 Shim 的加载信息 (使用全局变量 gEfiLoadedImageProtocolGuid)
    Status = BS->OpenProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid,
                              (VOID **)&LoadedImage, ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        console_print(L"Cannot get LoadedImage: %r\n", Status);
        return Status;
    }

    // 2. 打开文件系统 (使用全局变量 gEfiSimpleFileSystemProtocolGuid)
    Status = BS->OpenProtocol(LoadedImage->DeviceHandle,
                              &gEfiSimpleFileSystemProtocolGuid,
                              (VOID **)&FileSystem, ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status)) {
        console_print(L"Cannot open FileSystem: %r\n", Status);
        return Status;
    }

    // 3. 打开根目录
    Status = FileSystem->OpenVolume(FileSystem, &RootDir);
    if (EFI_ERROR(Status)) {
        console_print(L"Cannot open RootDir: %r\n", Status);
        return Status;
    }

    // 4. 打开驱动文件
    Status = RootDir->Open(RootDir, &FileHandle, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        return EFI_NOT_FOUND;
    }

    // 5. 获取文件大小 (使用全局变量 gEfiFileInfoGuid)
    Status = BS->AllocatePool(EfiBootServicesData, FileInfoSize, (VOID **)&FileInfo);
    if (EFI_ERROR(Status)) goto cleanup;

    Status = FileHandle->GetInfo(FileHandle, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if (EFI_ERROR(Status)) goto cleanup;
    
    FileSize = FileInfo->FileSize;
    BS->FreePool(FileInfo);
    FileInfo = NULL;

    // 6. 读取文件
    Status = BS->AllocatePool(EfiBootServicesData, FileSize, &FileBuffer);
    if (EFI_ERROR(Status)) goto cleanup;

    Status = FileHandle->Read(FileHandle, &FileSize, FileBuffer);
    if (EFI_ERROR(Status)) {
        BS->FreePool(FileBuffer);
        goto cleanup;
    }

    // 7. 加载并启动驱动
    Status = BS->LoadImage(FALSE, ImageHandle, NULL, FileBuffer, FileSize, &DriverHandle);
    BS->FreePool(FileBuffer); 

    if (EFI_ERROR(Status)) {
        console_print(L"LoadImage failed for %s: %r\n", FileName, Status);
        goto cleanup;
    }

    Status = BS->StartImage(DriverHandle, NULL, NULL);
    if (EFI_ERROR(Status)) {
        console_print(L"StartImage failed for %s: %r\n", FileName, Status);
        goto cleanup;
    }

    console_print(L"Driver %s loaded successfully.\n", FileName);
    Status = EFI_SUCCESS;

cleanup:
    if (FileInfo) BS->FreePool(FileInfo);
    if (FileHandle) FileHandle->Close(FileHandle);
    return Status;
}

// =================================================================
// 3. 主请求函数 (保持不变)
// =================================================================

EFI_STATUS 
send_http_get_request(EFI_HANDLE image_handle, CHAR8 *uri)
{
    EFI_STATUS efi_status;
    EFI_STATUS last_error = EFI_NOT_FOUND;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    BOOLEAN got_response = FALSE;
    VOID *dummy_ptr = NULL;

    if (uri == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    // --- 1. 确保 TCP 加载 ---
    efi_status = BS->LocateProtocol(&EFI_TCP_BINDING_GUID, NULL, &dummy_ptr);
    if (EFI_ERROR(efi_status)) {
        console_print(L"TCP missing. Trying to load from disk \\EFI\\BOOT\\TcpDxe.efi...\n");
        efi_status = LoadDriverFromFile(image_handle, L"\\EFI\\BOOT\\TcpDxe.efi");
        if (EFI_ERROR(efi_status)) {
            console_print(L"Failed to load TcpDxe.efi: %r\n", efi_status);
            return efi_status;
        }
    }

    // --- 2. 确保 HTTP 加载 ---
    efi_status = BS->LocateProtocol(&EFI_HTTP_BINDING_GUID, NULL, &dummy_ptr);
    if (EFI_ERROR(efi_status)) {
        console_print(L"HTTP missing. Trying to load from \\EFI\\BOOT\\HttpDxe.efi...\n");
        LoadDriverFromFile(image_handle, L"\\EFI\\BOOT\\HttpDxe.efi");
    }

    // --- 步骤 3: 查找网卡 ---
    efi_status = BS->LocateHandleBuffer(ByProtocol, &EFI_IP4_CONFIG2_GUID,
                                        NULL, &count, &handles);
    if (EFI_ERROR(efi_status) || count == 0) {
        perror(L"Failed to find any network interfaces (IP4_CONFIG2): %r\n", efi_status);
        return EFI_NOT_FOUND;
    } 

    console_print(L"Found %u network interface(s)\n", count);

    for (UINTN i = 0; i < count; i++) {
        EFI_HANDLE current_handle = handles[i];
        EFI_IP4_CONFIG2_PROTOCOL *ip4_cfg2_protocol = NULL;
        EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info = NULL;

        efi_status = BS->OpenProtocol(current_handle, &EFI_IP4_CONFIG2_GUID,
                                      (void **)&ip4_cfg2_protocol, image_handle, NULL, 
                                      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(efi_status)) {
            goto next_handle;
        }

        efi_status = ip4_cfg2_get_data(ip4_cfg2_protocol, Ip4Config2DataTypeInterfaceInfo,
                                       (void **)&ip4_cfg2_iface_info);
        if (EFI_ERROR(efi_status) || ip4_cfg2_iface_info == NULL) {
            goto next_handle;
        }

        if (!check_ip4_addr(ip4_cfg2_iface_info)) {
            efi_status = ip4_cfg2_protocol->SetData(
                ip4_cfg2_protocol, Ip4Config2DataTypePolicy,
                sizeof(EFI_IP4_CONFIG2_POLICY),
                &(EFI_IP4_CONFIG2_POLICY){ Ip4Config2PolicyDhcp });
            
            if (!EFI_ERROR(efi_status)) {
                BS->FreePool(ip4_cfg2_iface_info);
                ip4_cfg2_iface_info = NULL;
                efi_status = wait_until_get_iface_info(ip4_cfg2_protocol, &ip4_cfg2_iface_info);
                if (EFI_ERROR(efi_status)) {
                    perror(L"DHCP failed: %r\n", efi_status);
                    goto next_handle;
                }
            }
        }

        // --- 步骤 4: 绑定驱动 ---
        efi_status = BS->OpenProtocol(current_handle, &EFI_HTTP_BINDING_GUID,
                                      &dummy_ptr, image_handle, NULL, 
                                      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        
        if (EFI_ERROR(efi_status)) {
            console_print(L"Binding drivers to interface...\n");
            BS->ConnectController(current_handle, NULL, NULL, TRUE);
        } else {
            BS->CloseProtocol(current_handle, &EFI_HTTP_BINDING_GUID, image_handle, NULL);
        }

        // --- 步骤 5: 最终验证 ---
        efi_status = BS->OpenProtocol(current_handle, &EFI_TCP_BINDING_GUID, 
                                      &dummy_ptr, image_handle, NULL, 
                                      EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(efi_status)) {
            console_print(L"CRITICAL: TCP still missing after Connect.\n");
            last_error = EFI_UNSUPPORTED;
            goto next_handle;
        }
        BS->CloseProtocol(current_handle, &EFI_TCP_BINDING_GUID, image_handle, NULL);

        // --- 步骤 6: 发送请求 ---
        VOID *data = NULL;
        UINT64 datasize = 0;

        console_print(L"Sending HTTP request...\n");
        efi_status = httpboot_fetch_buffer_uri(image_handle, current_handle,
                                               uri, &data, &datasize);
        if (EFI_ERROR(efi_status)) {
            perror(L"Failed to fetch buffer: %r\n", efi_status);
            last_error = efi_status;
            goto next_handle;
        }

        if (data && datasize > 0) {
            CHAR8 *safe_str = AllocatePool(datasize + 1);
            if (safe_str) {
                CopyMem(safe_str, data, datasize);
                safe_str[datasize] = '\0';
                console_print(L"Get http response body: %a\n", safe_str);
                FreePool(safe_str);
            }
            FreePool(data);
            got_response = TRUE;
            last_error = EFI_SUCCESS;
        }

next_handle:
        if (ip4_cfg2_iface_info) {
            BS->FreePool(ip4_cfg2_iface_info);
            ip4_cfg2_iface_info = NULL;
        }
        if (ip4_cfg2_protocol) {
            BS->CloseProtocol(current_handle, &EFI_IP4_CONFIG2_GUID,
                              image_handle, NULL);
        }
        if (got_response) {
            break;
        }
    }

    if (handles) {
        BS->FreePool(handles);
    }

    return got_response ? EFI_SUCCESS : last_error;
}



// EFI_STATUS
// efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE * systab){
//     EFI_STATUS efi_status;

//     InitializeLib(image_handle, systab);
//     console_print(L"Enter efi_main\n");

//     // `python3 -m http.server 8888` on isrc server
// 	// http_request_method = HttpMethodGet;	
// 	// CHAR8 uri[] = "http://10.20.173.8:8888/";
	
//     CHAR8 uri[] = "http://10.20.173.8:80/v1/keypair";    
// 	http_request_method = HttpMethodPost;
// 	tx_body_json="{\"algo\":\"sm2\",\"kms\":\"\",\"flow\":\"classic\"}";	

//     efi_status = send_http_get_request(image_handle, uri);
//     if (EFI_ERROR(efi_status)) {
//         perror(L"Failed to send http get request\n");
//         goto exit_main;
//     }

// exit_main:
//     return efi_status;
// }

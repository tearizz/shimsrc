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

/*
	Add Firmware_Volume_Protocol GUID
*/
static EFI_GUID gEfiFirmwareVolume2ProtocolGuid = { 0x220e73b6, 0x6bdb, 0x4413, { 0x84, 0x5, 0xb9, 0x74, 0xb1, 0x8, 0x61, 0x90 } };
typedef EFI_STATUS _EFI_FIRMWARE_VOLUME2_PROTOCOL EFI_FIRMWARE_VOLUME2_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_FV_READ_FILE) (
	IN struct _EFI_FIRMWARE_VOLUME2_PROTOCOL *This,
	IN EFI_GUID *NameGuid,
	IN OUT VOID **Buffer,
	IN OUT UINTN *BufferSize,
	OUT UINT32 *FoundType,
	OUT UINT32 *FileAttributes,
	OUT UINT32 *AuthenticationStatus
)

typedef struct _EFI_FIRMWARE_VOULME2_PROTOCOL {
	VOID *GetVolumnAttributes;
	VOID *SetVolumnAttributes;
	EFI_FV_READ_FILE ReadFile;
} EFI_FIRMWARE_VOLUME2_PROTOCOL;

/*
	从固件卷中读取指定的GUID的驱动并加载
	@param ImageHandle 		Shim的Image Handle
	@param DriverGuid		要加载的驱动文件的GUID

	@retval EFI_SUCCESS		驱动加载并启动成功
*/
EFI_STATUS
LoadDriverFromFirmware(EFI_HANDLE ImageHandle, EFI_GUID *DriverGuid)
{
	EFI_STATUS Status;
	UINTN NumHandles = 0;
	EFI_HANDLE *HandleBuffer = NULL;
	EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv = NULL;

	VOID *DriverBuffer = NULL;
	UINTN DriverSize = 0;
	UINTN32 AuthenticationStatus;

	EFI_HANDLE DriverHandle = NULL;

	// 1. Search all Firmware Volume Protocol
	Status = BS->LocateHandleBuffer(ByProtocol, &gEfiFirmwareVolume2ProtocolGuid,
		NULL, &NumHandles, &HandleBuffer);
	if (EFI_ERROR(Status) || NumHandles == 0) {
		console_print(L"Error: No Firmware Volume Protocol found.\n");
		return Status;
	}

	// 2. 遍历所有的固件卷
	for (UINTN i = 0; i < NumHandles; i++) {
		Status = BS->HandleProtocol(HandleBuffer[i], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
		if (EFI_ERROR(Status)) continue;

		// 3. Read drive files
		Status = Fv->ReadFile(
			Fv, DriverGuid, &DriverBuffer, &DriverSize, NULL, NULL, &AuthenticationStatus;)
		if (!EFI_ERROR(Status) && DriverBuffer && DriverSize > 0) {
			console_print(L"Found driver in FV (Size: %lu bytes). Loading...\n",DriverSize);

			// 4. Load driver into memory
			Status = BS->LoadImage(
				FALSE,
				ImageHandle,
				NULL,
				DriverBuffer,
				DriverSize,
				&DriverHandle
			);

			BS->FreePool(DriverBuffer);			

			if (EFI_ERROR(Status)) {
				console_print(L"LoadImage failed: %r\n",Status);
				continue;
			}

			// 5. Boot Driver
			Status = BS->StartImage(DriverHandle, NULL, NULL);
			if (!EFI_ERROR(Status)) {
				console_print(L"Driver started successfully.\n");
				BS->FreePool(HandleBuffer);
				return EFI_SUCCESS;
			} else {
				console_print(L"StartImage failed: %r\n",Status);
				BS->FreePool(HandleBuffer);
				return Status;
			}
		}
	}

	if (HandleBuffer) BS->FreePool(HandleBuffer);
	return EFI_NOT_FOUND
}

EFI_STATUS 
send_http_get_request(EFI_HANDLE image_handle, CHAR8 *uri)
{
	EFI_STATUS efi_status;
	EFI_STATUS last_error = EFI_NOT_FOUND;
	UINTN count=0;
	EFI_HANDLE *handles = NULL;
	BOOLEAN got_response = FALSE;
	VOID *dummy_ptr = NULL;

	if (uri == NULL) {
		return EFI_INVALID_PARAMETER;
	}

	efi_status = BS->LocateProtocol(&EFI_TCP_BINDING_GUID, NULL, &dummy_ptr);
	if (EFI_ERROR(efi_status)) {
		console_print(L"TCP Driver not in memory. Loading from Firmware...\n");
		efi_status = LoadDriverFromFirmware(image_handle,&EFI_TCP_FILE_GUID);
		if (EFI_ERROR(efi_status)) {
			console_print(L"Fatal: Could not load TcpDxe from firmware: %r\n", efi_status);
			return efi_status;
		}
	}
	
	efi_status = BS->LocateProtocol(&EFI_HTTP_BINDING_GUID, NULL, &dummy_ptr);
	if (EFI_ERROR(efi_status)) {
		console_print(L"HTTP Driver not in memory. Loading from Firmware...\n");
		LoadDriverFromFirmware(image_handle, &EFI_HTTP_FILE_GUID);
	}

	// Construct Network from bottom to top.
	// Search IP4_CONFIG2 Protocol first.
	efi_status = BS->LocateHandleBuffer(ByProtocol, &EFI_IP4_CONFIG2_GUID,
		NULL, &count, &handles);
	if (EFI_ERROR(efi_status) || count == 0) {
		perror(L"Failed to find any network interfaces (IP4_CONFIG2): %r\n", efi_status);
		return EFI_NOT_FOUND;
	} 

	console_print(L"Found %u network interface(s)\n",count);

	for (UINTN i = 0; i < count; i++) {
		EFI_HANDLE current_handle = handles[i];
		EFI_IP4_CONFIG2_PROTOCOL *ip4_cfg2_protocol = NULL;
		EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info = NULL;

		// Open IP4_CONFIG2 protocol
		efi_status = BS->OpenProtocol(current_handle, &EFI_IP4_CONFIG2_GUID,
			(void **)&ip4_cfg2_protocol, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(efi_status)) {
			goto next_handle;
		}

		// Check IP address and DHCP logic
		efi_status = ip4_cfg2_get_data(ip4_cfg2_protocol, Ip4Config2DataTypeInterfaceInfo,
			(void **)&ip4_cfg2_iface_info);
		if (EFI_ERROR(efi_status) || ip4_cfg2_iface_info == NULL) {
			goto next_handle;
		}

		if (!check_ip4_addr(ip4_cfg2_iface_info)) {
			// Try DHCP
			efi_status = ip4_cfg2_protocol -> SetData (
				ip4_cfg2_protocol, Ip4Config2DataTypePolicy,
				sizeof(EFI_IP4_CONFIG2_POLICY),
				&(EFI_IP4_CONFIG2_POLICY){ Ip4Config2PolicyDhcp});
			
			if (EFI_ERROR(efi_status)) {
				BS->FreePool(ip4_cfg2_iface_info);
				ip4_cfg2_iface_info = NULL;
				efi_status = wait_until_get_iface_info(ip4_cfg2_protocol, &ip4_cfg2_iface_info);
				if (EFI_ERROR(efi_status)) {
					goto next_handle;
				}
			}
		}

		// CHeck if handle support HTTP_SERVICE_BINDING
		efi_status = BS->OpenProtocol(current_handle, &EFI_HTTP_BINDING_GUID,
			&dummy_ptr, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(efi_status)) {
			console_print(L"HTTP not found. Forcing driver stack connection...\n");
			BS->ConnectController(current_handle, NULL, NULL, TRUE);
		} else{
			BS->CloseProtocol(current_handle, &EFI_HTTP_BINDING_GUID, image_handle, NULL);
		}

		efi_status = BS->OpenProtocol(current_handle, &EFI_TCP_BINDING_GUID, 
			&dummy_ptr, image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
		if (EFI_ERROR(efi_status)) {
			console_print(L"TCP not bound to this interface. Forcing connection...\n");
			console_print(L"This means TcpDxe is NOT in my firmware or failed to load.\n");
			last_error = EFI_UNSUPPORTED;
			goto next_handle;
		}
		BS->CloseProtocol(current_handle, &EFI_TCP_BINDING_GUID, image_handle, NULL);

		// Start sending request
		VOID *data = NULL;
		UINT64 datasize = 0;

		efi_status = httpboot_fetch_buffer_uri(image_handle, current_handle,
			uri, &data, &datasize);
		if (EFI_ERROR(efi_status)) {
			perror(L"Failed to fetch buffer: %r\n",efi_status);
			last_error = efi_status;
			goto next_handle;
		}

		// Receive response
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
// send_http_get_request(EFI_HANDLE image_handle, CHAR8 *uri)
// {
// 	EFI_STATUS efi_status;

//     BOOLEAN got_response = FALSE;
//     EFI_STATUS last_error = EFI_NOT_FOUND;

// 	UINTN count = 0;
// 	EFI_HANDLE *http_binding_handles = NULL;

//     if (uri == NULL) {
//         return EFI_INVALID_PARAMETER;
//     }

// 	efi_status = BS->LocateHandleBuffer(ByProtocol, &EFI_HTTP_BINDING_GUID,
// 	                                    NULL, &count, &http_binding_handles);
// 	if (EFI_ERROR(efi_status)) {
// 		perror(L"Failed to get http binding handles: %r\n", efi_status);
// 		return efi_status;
// 	}
// 	if (count == 0 || http_binding_handles == NULL) {
//         if (http_binding_handles) {
//             BS->FreePool(http_binding_handles);
//         }
// 		return EFI_NOT_FOUND;
// 	}
    
//     console_print(L"Found %u HTTP binding handle(s)\n", count);

// 	for (UINTN i = 0; i < count; i++) {
// 		efi_status =
// 			print_device_path(image_handle, http_binding_handles[i]);
// 		if (EFI_ERROR(efi_status)) {
// 			perror(L"Failed to print device path\n");
// 			goto next_handle;
// 		}
// 		EFI_IP4_CONFIG2_PROTOCOL *ip4_cfg2_protocol = NULL;
// 		efi_status = BS->OpenProtocol(http_binding_handles[i],
// 		                              &EFI_IP4_CONFIG2_GUID,
// 		                              (void **)&ip4_cfg2_protocol,
// 		                              image_handle, NULL,
// 		                              EFI_OPEN_PROTOCOL_GET_PROTOCOL);
// 		if (EFI_ERROR(efi_status) || ip4_cfg2_protocol == NULL) {
// 			perror(L"Failed to open ip4 config2 protorol: %r\n",efi_status);
//             last_error = efi_status;
// 			goto next_handle;
// 		}
// 		EFI_IP4_CONFIG2_INTERFACE_INFO *ip4_cfg2_iface_info = NULL;
// 		efi_status = ip4_cfg2_get_data(ip4_cfg2_protocol,
// 		                               Ip4Config2DataTypeInterfaceInfo,
// 		                               (void **)&ip4_cfg2_iface_info);
// 		if (EFI_ERROR(efi_status) || ip4_cfg2_iface_info == NULL) {
// 			perror(L"Failed to open ip4 config2 protorol: %r\n",efi_status);
//             last_error = efi_status;
// 			goto next_handle;
// 		}

// 		if (check_ip4_addr(ip4_cfg2_iface_info)) {
// 			print_ip4_addr_verbose(ip4_cfg2_iface_info);
// 		} else {
// 			efi_status = ip4_cfg2_protocol->SetData(
// 				ip4_cfg2_protocol, Ip4Config2DataTypePolicy,
// 				sizeof(EFI_IP4_CONFIG2_POLICY),
// 				&(EFI_IP4_CONFIG2_POLICY){
// 					Ip4Config2PolicyDhcp });
// 			if (EFI_ERROR(efi_status)) {
// 				perror(L"Failed to set DHCP policy: %r\n", efi_status);
//                 last_error = efi_status;
// 				goto next_handle;
// 			}
// 			// Loop until get ip.
// 			// efi_status = wait_until_get_iface_info(
// 			// 	ip4_cfg2_protocol, &ip4_cfg2_iface_info);
// 			// if (EFI_ERROR(efi_status)) {
// 			// 	perror(L"Failed to get ip4 addr by DHCP\n");
// 			// 	goto break_loop;
// 			// }
            
//             BS->FreePool(ip4_cfg2_iface_info);
//             ip4_cfg2_iface_info=NULL;
            
//             efi_status = wait_until_get_iface_info(ip4_cfg2_protocol, &ip4_cfg2_iface_info);
            
//             if (EFI_ERROR(efi_status)) {
//                 perror(L"Failed to get IP4 addr by DHCP: %r\n",efi_status);
//                 last_error = efi_status;
//                 goto next_handle;
//             }
// 		}
// 		void *data = NULL;
//         // seems auto append.
// 		UINT64 datasize = 0;
// 		efi_status = httpboot_fetch_buffer_uri(image_handle,
// 		                                       http_binding_handles[i],
// 		                                       uri, &data, &datasize);
// 		if (EFI_ERROR(efi_status)) {
// 			perror(L"Failed to fetch image: %r\n", efi_status);
//             last_error = efi_status;
// 			goto next_handle;
// 		}
// 	if (data && datasize > 0) {
// 		CHAR8 *safe_str = AllocatePool(datasize + 1);
// 		if (safe_str) {
// 			CopyMem(safe_str, data, datasize);
// 			safe_str[datasize] = '\0';
// 			console_print(L"Get http response body:%a\n", safe_str);
// 			FreePool(safe_str);
// 		}
// 	}
    
//     got_response = TRUE;
//     last_error = EFI_SUCCESS;

// next_handle:
//     if(data){
//         BS->FreePool(data);
//         data = NULL;
//     }
//     if (ip4_cfg2_iface_info) {
//         BS->FreePool(ip4_cfg2_iface_info);
//         ip4_cfg2_iface_info = NULL;
//     }
    
//     if (ip4_cfg2_protocol) {
//         BS->CloseProtocol(http_binding_handles[i],
//                 &EFI_IP4_CONFIG2_GUID,
//                 image_handle, NULL);
//         ip4_cfg2_protocol = NULL;
//     }
    
//     if (got_response) {
//         break;
//     }
// }
    
//     if (http_binding_handles) {
//         BS->FreePool(http_binding_handles);
//         http_binding_handles = NULL;
//     }

// 	return got_response ? EFI_SUCCESS : last_error;
// }



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

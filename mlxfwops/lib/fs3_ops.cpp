/*
 * Copyright (C) Jan 2013 Mellanox Technologies Ltd. All rights reserved.
 * 
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 * 
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 * 
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 * 
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include <stdlib.h>
#include <algorithm>
#include <vector>

#include <tools_utils.h>
#include <bit_slice.h>
#include <mtcr.h>
#include <reg_access/reg_access.h>

#include "fs3_ops.h"

#define FS3_FLASH_SIZE 0x400000
#define FS3_LOG_CHUNK_SIZE 21

#define FS3_DFLT_GUID_NUM_TO_ALLOCATE 8
#define FS3_DFLT_GUID_STEP 1

#define DEFAULT_GUID_NUM 0xff
#define DEFAULT_STEP DEFAULT_GUID_NUM

// FW Binary version

// max supported major version
// first time breaking compatibility remove the check if the version is not initialized
#define FS3_MAX_BIN_VER_MAJOR 1

// min supported version
#define FS3_MIN_BIN_VER_MAJOR 1
#define FS3_MIN_BIN_VER_MINOR 1

const u_int32_t Fs3Operations::_itocSignature[4] = {
        ITOC_ASCII,   // Ascii of "MTFW"
        TOC_RAND1,   // Random data
        TOC_RAND2,
        TOC_RAND3
};

const Fs3Operations::SectionInfo Fs3Operations::_fs3SectionsInfoArr[] = {
    {FS3_END,           "END"},
    {FS3_ITOC,          "ITOC_Header"},

    {FS3_BOOT_CODE,     "BOOT_CODE"},
    {FS3_PCI_CODE,      "PCI_CODE"},
    {FS3_MAIN_CODE,     "MAIN_CODE"},
    {FS3_PCIE_LINK_CODE, "PCIE_LINK_CODE"},
    {FS3_IRON_PREP_CODE, "IRON_PREP_CODE"},
    {FS3_POST_IRON_BOOT_CODE, "POST_IRON_BOOT_CODE"},
    {FS3_UPGRADE_CODE,  "UPGRADE_CODE"},
    {FS3_HW_BOOT_CFG,   "HW_BOOT_CFG"},
    {FS3_HW_MAIN_CFG,   "HW_MAIN_CFG"},
    {FS3_PHY_UC_CODE,   "PHY_UC_CODE"},
    {FS3_PHY_UC_CONSTS, "PHY_UC_CONSTS"},
    {FS3_IMAGE_INFO,    "IMAGE_INFO"},
    {FS3_FW_BOOT_CFG,   "FW_BOOT_CFG"},
    {FS3_FW_MAIN_CFG,   "FW_MAIN_CFG"},
    {FS3_ROM_CODE,      "ROM_CODE"},
    {FS3_RESET_INFO,    "FS3_RESET_INFO"},
    {FS3_DBG_FW_INI,    "DBG_FW_INI"},
    {FS3_DBG_FW_PARAMS, "DBG_FW_PARAMS"},
    {FS3_FW_ADB,        "FW_ADB"},

    {FS3_MFG_INFO,      MFG_INFO},
    {FS3_DEV_INFO,      "DEV_INFO"},
    {FS3_NV_DATA1,      "NV_DATA"},
    {FS3_VPD_R0,        "VPD_R0"},
    {FS3_NV_DATA2,      "NV_DATA"},
    {FS3_NV_DATA0,      "NV_DATA"},
    {FS3_FW_NV_LOG,     "FW_NV_LOG"},
    {FS3_NV_DATA0,      "NV_DATA"},
};

bool Fs3Operations::Fs3UpdateImgCache(u_int8_t *buff, u_int32_t addr, u_int32_t size)
{
    // vector<u_int8_t>::iterator it;
    u_int32_t min_required_size = addr + size;

    if (_fs3ImgInfo.imageCache.size() < min_required_size) {
        _fs3ImgInfo.imageCache.resize(min_required_size);
    }
    for (u_int32_t i = 0; i < size; i++) {
        _fs3ImgInfo.imageCache.at(addr + i) = buff[i];
    }
    return true;
}

bool Fs3Operations::UpdateImgCache(u_int8_t *buff, u_int32_t addr, u_int32_t size)
{
    return Fs3UpdateImgCache(buff, addr, size);
}

const char *Fs3Operations::GetSectionNameByType(u_int8_t section_type) {

    for (u_int32_t i = 0; i < ARR_SIZE(_fs3SectionsInfoArr); i++) {
        const SectionInfo *sect_info = &_fs3SectionsInfoArr[i];
        if (sect_info->type == section_type) {
            return sect_info->name;
        }
    }
    return UNKNOWN_SECTION;
}

bool Fs3Operations::DumpFs3CRCCheck(u_int8_t sect_type, u_int32_t sect_addr, u_int32_t sect_size, u_int32_t crc_act,
        u_int32_t crc_exp, bool ignore_crc, VerifyCallBack verifyCallBackFunc)
{
    char pr[256];
    const char *sect_type_str = GetSectionNameByType(sect_type);
    sprintf(pr, CRC_CHECK_OLD, PRE_CRC_OUTPUT, sect_addr, sect_addr + sect_size - 1, sect_size,
            sect_type_str);
    if (!strcmp(sect_type_str, UNKNOWN_SECTION)) {
        sprintf(pr + strlen(pr), ":0x%x", sect_type);
    }
    sprintf(pr + strlen(pr), ")");
    return  CheckAndPrintCrcRes(pr, 0, sect_addr, crc_exp, crc_act, ignore_crc, verifyCallBackFunc);

}

bool Fs3Operations::CheckTocSignature(struct cibfw_itoc_header *itoc_header, u_int32_t first_signature)
{
    if ( itoc_header->signature0 != first_signature  ||
         itoc_header->signature1 !=  TOC_RAND1       ||
         itoc_header->signature2 !=  TOC_RAND2       ||
         itoc_header->signature3 !=  TOC_RAND3) {
        return false;
    }
    return true;
}

#define CHECK_UID_STRUCTS_SIZE(uids_context, cibfw_guids_context) {\
	    if (sizeof(uids_context) != sizeof(cibfw_guids_context)) {\
	        return errmsg("Internal error: Size of uids_t (%d) is not equal to size of  struct cibfw_guids guids (%d)\n",\
	                (int)sizeof(uids_context), (int)sizeof(cibfw_guids_context));\
	    }\
}

#define CHECK_MFG_NEW_FORMAT(mfg_st)\
        (mfg_st.major_version == 1)
#define CHECK_MFG_OLD_FORMAT(mfg_st)\
        (mfg_st.major_version == 0)
bool Fs3Operations::GetMfgInfo(u_int8_t *buff)
{
    // structs of the same size we can unpack either way
    struct cibfw_mfg_info cib_mfg_info;
    struct cx4fw_mfg_info cx4_mfg_info;

    cibfw_mfg_info_unpack(&cib_mfg_info, buff);
    // cibfw_mfg_info_dump(&mfg_info, stdout);
    if (CHECK_MFG_NEW_FORMAT(cib_mfg_info)) {
        cx4fw_mfg_info_unpack(&cx4_mfg_info, buff);
        CHECK_UID_STRUCTS_SIZE(_fs3ImgInfo.ext_info.orig_fs3_uids_info.cx4_uids, cx4_mfg_info.guids);
        memcpy(&_fs3ImgInfo.ext_info.orig_fs3_uids_info.cx4_uids, &cx4_mfg_info.guids, sizeof(cx4_mfg_info.guids));
        strcpy(_fs3ImgInfo.ext_info.orig_psid, cx4_mfg_info.psid);
        _fs3ImgInfo.ext_info.guids_override_en = cx4_mfg_info.guids_override_en;
        _fs3ImgInfo.ext_info.orig_fs3_uids_info.valid_field = 1;
    } else if (CHECK_MFG_OLD_FORMAT(cib_mfg_info)){
        CHECK_UID_STRUCTS_SIZE(_fs3ImgInfo.ext_info.orig_fs3_uids_info.cib_uids, cib_mfg_info.guids);
        memcpy(&_fs3ImgInfo.ext_info.orig_fs3_uids_info.cib_uids, &cib_mfg_info.guids, sizeof(cib_mfg_info.guids));
        strcpy(_fs3ImgInfo.ext_info.orig_psid, cib_mfg_info.psid);
        _fs3ImgInfo.ext_info.guids_override_en = cib_mfg_info.guids_override_en;
        _fs3ImgInfo.ext_info.orig_fs3_uids_info.valid_field = 0;
    } else {
        return errmsg(MLXFW_UNKNOWN_SECT_VER_ERR, "Unknown MFG_INFO format version (%d.%d).", cib_mfg_info.major_version, cib_mfg_info.minor_version);
    }
    return true;

}

#define GET_IMAGE_INFO_VERSION(imageInfoBuff, major, minor) \
        u_int32_t _IIVerDw = __be32_to_cpu(*(u_int32_t*)imageInfoBuff);\
        minor = ((_IIVerDw) >> 16) & 0xff;\
        major = ((_IIVerDw) >> 24) & 0xff;
#define CHECK_IMAGE_INFO_VERSION(major)\
    ((major) == 0)

#define FAIL_NO_OCR(str) do { \
                        if (_ioAccess->is_flash() && _fwParams.ignoreCacheRep == 0) {\
                            return errmsg(MLXFW_OCR_ERR, "-ocr flag must be specified for %s operation.", str);\
                        }\
                    } while (0)

bool Fs3Operations::GetImageInfo(u_int8_t *buff)
{
    struct cibfw_image_info image_info;
    int IIMajor, IIMinor;

    // TODO: adrianc: use the version fields once they are available in tools layouts
    GET_IMAGE_INFO_VERSION(buff, IIMajor, IIMinor);
    (void)IIMinor;
    if (!CHECK_IMAGE_INFO_VERSION(IIMajor)) {
        return errmsg(MLXFW_UNKNOWN_SECT_VER_ERR, "Unknown IMAGE_INFO format version (%d.%d).", IIMajor, IIMinor);
    }
    cibfw_image_info_unpack(&image_info, buff);
    // cibfw_image_info_dump(&image_info, stdout);

    _fwImgInfo.ext_info.fw_ver[0] = image_info.FW_VERSION.MAJOR;
    _fwImgInfo.ext_info.fw_ver[1] = image_info.FW_VERSION.MINOR;
    _fwImgInfo.ext_info.fw_ver[2] = image_info.FW_VERSION.SUBMINOR;

    _fwImgInfo.ext_info.mic_ver[0] = image_info.mic_version.MAJOR;
    _fwImgInfo.ext_info.mic_ver[1] = image_info.mic_version.MINOR;
    _fwImgInfo.ext_info.mic_ver[2] = image_info.mic_version.SUBMINOR;

    _fwImgInfo.ext_info.fw_rel_date[0] = (u_int16_t)image_info.FW_VERSION.Day;
    _fwImgInfo.ext_info.fw_rel_date[1] = (u_int16_t)image_info.FW_VERSION.Month;
    _fwImgInfo.ext_info.fw_rel_date[2] = (u_int16_t)image_info.FW_VERSION.Year;

    // assuming number of supported_hw_id < MAX_NUM_SUPP_HW_IDS
    memcpy(_fwImgInfo.supportedHwId, image_info.supported_hw_id, sizeof(image_info.supported_hw_id));
    _fwImgInfo.supportedHwIdNum = (sizeof(image_info.supported_hw_id))/sizeof(image_info.supported_hw_id[0]);

    strcpy(_fs3ImgInfo.ext_info.image_vsd, image_info.vsd);
    strcpy(_fwImgInfo.ext_info.psid, image_info.psid);
    strcpy(_fwImgInfo.ext_info.product_ver, image_info.prod_ver);
    return true;
}

#define CHECK_DEV_INFO_NEW_FORMAT(info_st)\
        (info_st.major_version == 2)
#define CHECK_DEV_INFO_OLD_FORMAT(info_st)\
        (info_st.major_version == 1)
bool Fs3Operations::GetDevInfo(u_int8_t *buff)
{
    struct cibfw_device_info cib_dev_info;
    struct cx4fw_device_info cx4_dev_info;
    // same size, we can unpack to check version
    cibfw_device_info_unpack(&cib_dev_info, buff);
    // cibfw_device_info_dump(&dev_info, stdout);

    if (CHECK_DEV_INFO_NEW_FORMAT(cib_dev_info)) {
        cx4fw_device_info_unpack(&cx4_dev_info, buff);
        CHECK_UID_STRUCTS_SIZE(_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids, cx4_dev_info.guids);
        memcpy(&_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids, &cx4_dev_info.guids, sizeof(cx4_dev_info.guids));
        strcpy(_fwImgInfo.ext_info.vsd, cx4_dev_info.vsd);
        _fs3ImgInfo.ext_info.fs3_uids_info.valid_field = 1;
        _fwImgInfo.ext_info.vsd_sect_found = true;
    } else if (CHECK_DEV_INFO_OLD_FORMAT(cib_dev_info)){
        CHECK_UID_STRUCTS_SIZE(_fs3ImgInfo.ext_info.fs3_uids_info.cib_uids, cib_dev_info.guids);
        memcpy(&_fs3ImgInfo.ext_info.fs3_uids_info.cib_uids, &cib_dev_info.guids, sizeof(cib_dev_info.guids));
        strcpy(_fwImgInfo.ext_info.vsd, cib_dev_info.vsd);
        _fs3ImgInfo.ext_info.fs3_uids_info.valid_field = 0;
        _fwImgInfo.ext_info.vsd_sect_found = true;
    } else {
        return errmsg(MLXFW_UNKNOWN_SECT_VER_ERR, "Unknown DEV_INFO format version (%d.%d).", cib_dev_info.major_version, cib_dev_info.minor_version);
    }
    return true;
}

bool Fs3Operations::GetRomInfo(u_int8_t *buff, u_int32_t size)
{
    TOCPUn(buff, size/4);
    // update _romSect buff
    GetSectData(_romSect, (u_int32_t*)buff, size);
    // parse rom Info and fill rom_info struct
    RomInfo rInfo(_romSect);
    rInfo.ParseInfo();
    rInfo.initRomsInfo(&_fwImgInfo.ext_info.roms_info);
    return true;
}

bool Fs3Operations::GetImageInfoFromSection(u_int8_t *buff, u_int8_t sect_type, u_int32_t sect_size, u_int8_t check_support_only)
{
    #define EXEC_GET_INFO_OR_GET_SUPPORT(get_info_func, buff, check_support_only) (check_support_only) ? true : get_info_func(buff);

    switch (sect_type) {
        case FS3_MFG_INFO:
            return EXEC_GET_INFO_OR_GET_SUPPORT(GetMfgInfo, buff, check_support_only);
        case FS3_IMAGE_INFO:
            return EXEC_GET_INFO_OR_GET_SUPPORT(GetImageInfo, buff, check_support_only);
        case FS3_DEV_INFO:
            return EXEC_GET_INFO_OR_GET_SUPPORT(GetDevInfo, buff, check_support_only);
        case FS3_ROM_CODE:
            return check_support_only ? true : GetRomInfo(buff, sect_size);
        default:
            break;
    }

    if (check_support_only) {
        return false;
    }
    return errmsg("Getting info from section type (%s:%d) is not supported\n", GetSectionNameByType(sect_type), sect_type);
}

bool Fs3Operations::IsGetInfoSupported(u_int8_t sect_type)
{
    return GetImageInfoFromSection((u_int8_t*)NULL, sect_type, 0, 1);
}

bool Fs3Operations::IsFs3SectionReadable(u_int8_t type, QueryOptions queryOptions)
{
    //printf("-D- readSectList.size %d\n", (int) _readSectList.size());
    if (_readSectList.size()) {
        for (u_int32_t i = 0; i < _readSectList.size(); i++) {
            if (_readSectList.at(i) == type) {
                return true;
            }
        }
        return false;

    } else if (queryOptions.quickQuery) {
        if ( IsGetInfoSupported(type)) {
            return true;
        }
        return false;
    }
    return true;
}

bool Fs3Operations::VerifyTOC(u_int32_t dtoc_addr, bool& bad_signature, VerifyCallBack verifyCallBackFunc, bool show_itoc,
        struct QueryOptions queryOptions)
{
    u_int8_t buffer[TOC_HEADER_SIZE], entry_buffer[TOC_ENTRY_SIZE];
    struct cibfw_itoc_header itoc_header;
    bool ret_val = true, mfg_exists = false;
    u_int32_t phys_addr;
    bad_signature = false;


    // Read the sigmature and check it
    READBUF((*_ioAccess), dtoc_addr, buffer, TOC_HEADER_SIZE, "TOC Header");
    Fs3UpdateImgCache(buffer, dtoc_addr, TOC_HEADER_SIZE);
    cibfw_itoc_header_unpack(&itoc_header, buffer);
    memcpy(_fs3ImgInfo.itocHeader, buffer, CIBFW_ITOC_HEADER_SIZE);
    // cibfw_itoc_header_dump(&itoc_header, stdout);
    u_int32_t first_signature =  ITOC_ASCII;
    if (!CheckTocSignature(&itoc_header, first_signature)) {
        bad_signature = true;
        return false;
    }
    u_int32_t toc_crc = CalcImageCRC((u_int32_t*)buffer, (TOC_HEADER_SIZE / 4) - 1);
    phys_addr = _ioAccess->get_phys_from_cont(dtoc_addr, _fwImgInfo.cntxLog2ChunkSize, _fwImgInfo.imgStart != 0);
    if (!DumpFs3CRCCheck(FS3_ITOC, phys_addr, TOC_HEADER_SIZE, toc_crc, itoc_header.itoc_entry_crc,false,verifyCallBackFunc)) {
        ret_val = false;
    }
    _fs3ImgInfo.itocAddr = dtoc_addr;

    int section_index = 0;
    struct cibfw_itoc_entry toc_entry;

    do {
        // Uopdate the cont address
        _ioAccess->set_address_convertor(_fwImgInfo.cntxLog2ChunkSize, _fwImgInfo.imgStart != 0);
        u_int32_t entry_addr = dtoc_addr + TOC_HEADER_SIZE + section_index *  TOC_ENTRY_SIZE;
        READBUF((*_ioAccess), entry_addr , entry_buffer, TOC_ENTRY_SIZE, "TOC Entry");
        Fs3UpdateImgCache(entry_buffer, entry_addr, TOC_ENTRY_SIZE);

        cibfw_itoc_entry_unpack(&toc_entry, entry_buffer);
        if (toc_entry.type == FS3_MFG_INFO) {
            mfg_exists = true;
        }
        // printf("-D- toc = %#x, toc_entry.type  = %#x\n", section_index, toc_entry.type);
        if (toc_entry.type != FS3_END) {
            if (section_index + 1 >= MAX_TOCS_NUM) {
                return errmsg("Internal error: number of ITOCs %d is greater than allowed %d", section_index + 1, MAX_TOCS_NUM);
            }

            u_int32_t entry_crc = CalcImageCRC((u_int32_t*)entry_buffer, (TOC_ENTRY_SIZE / 4) - 1);
            u_int32_t entry_size_in_bytes = toc_entry.size * 4;
            //printf("-D- entry_crc = %#x, toc_entry.itoc_entry_crc = %#x\n", entry_crc, toc_entry.itoc_entry_crc);

            if (toc_entry.itoc_entry_crc == entry_crc) {
                // Update last image address
                u_int32_t section_last_addr;
                u_int32_t flash_addr = toc_entry.flash_addr << 2;
                if (!toc_entry.relative_addr) {
                    _ioAccess->set_address_convertor(0, 0);
                    phys_addr = flash_addr;
                    _fs3ImgInfo.smallestAbsAddr = (_fs3ImgInfo.smallestAbsAddr < flash_addr && _fs3ImgInfo.smallestAbsAddr > 0)
                            ? _fs3ImgInfo.smallestAbsAddr : flash_addr;
                } else {
                    phys_addr = _ioAccess->get_phys_from_cont(flash_addr, _fwImgInfo.cntxLog2ChunkSize, _fwImgInfo.imgStart != 0);
                    u_int32_t currSizeOfImgdata = phys_addr + entry_size_in_bytes;
                    _fs3ImgInfo.sizeOfImgData = (_fs3ImgInfo.sizeOfImgData > currSizeOfImgdata) ? _fs3ImgInfo.sizeOfImgData : phys_addr;
                }
                section_last_addr = phys_addr + entry_size_in_bytes;
                _fwImgInfo.lastImageAddr = (_fwImgInfo.lastImageAddr >= phys_addr + section_last_addr) ? _fwImgInfo.lastImageAddr : section_last_addr;

                if (IsFs3SectionReadable(toc_entry.type, queryOptions)) {

                    // Only when we have full verify or the info of this section should be collected for query
                    std::vector<u_int8_t> buffv(entry_size_in_bytes);
                    u_int8_t *buff = (u_int8_t*)(&(buffv[0]));

                    if (show_itoc) {
                        cibfw_itoc_entry_dump(&toc_entry, stdout);
                        DumpFs3CRCCheck(toc_entry.type, phys_addr, entry_size_in_bytes, 0, 0, true, verifyCallBackFunc);
                    } else {
                        READBUF((*_ioAccess), flash_addr, buff, entry_size_in_bytes, "Section");
                        Fs3UpdateImgCache(buff, flash_addr, entry_size_in_bytes);
                        u_int32_t sect_crc = CalcImageCRC((u_int32_t*)buff, toc_entry.size);

                       // printf("-D- flash_addr: %#x, toc_entry_size = %#x, actual sect = %#x, from itoc: %#x np_crc = %s\n", flash_addr, toc_entry.size, sect_crc,
                       //         toc_entry.section_crc, toc_entry.no_crc ? "yes" : "no");
                        if (!DumpFs3CRCCheck(toc_entry.type, phys_addr, entry_size_in_bytes, sect_crc, toc_entry.section_crc, toc_entry.no_crc, verifyCallBackFunc)) {
                            if (toc_entry.device_data) {
                                _badDevDataSections = true;
                            }
                            ret_val = false;
                        } else {
                            //printf("-D- toc type : 0x%.8x\n" , toc_entry.type);
                            GetSectData(_fs3ImgInfo.tocArr[section_index].section_data, (u_int32_t*)buff, toc_entry.size * 4);
                            if (IsGetInfoSupported(toc_entry.type)) {
                                 if (!GetImageInfoFromSection(buff, toc_entry.type, toc_entry.size * 4)) {
                                     ret_val = false;
                                     errmsg("Failed to get info from section %d", toc_entry.type);
                                 }
                            } else if (toc_entry.type == FS3_DBG_FW_INI) {
                                 TOCPUn(buff, toc_entry.size);
                                 GetSectData(_fwConfSect, (u_int32_t*)buff, toc_entry.size * 4);
                            }
                        }
                    }
                 }
             } else {
                 /*
                  printf("-D- Bad ITOC CRC: toc_entry.itoc_entry_crc = %#x, actual crc: %#x, entry_size_in_bytes = %#x\n", toc_entry.itoc_entry_crc,
                         entry_crc, entry_size_in_bytes);
                  */
                 return errmsg(MLXFW_BAD_CRC_ERR, "Bad Itoc Entry CRC. Expected: 0x%x , Actual: 0x%x", toc_entry.itoc_entry_crc, entry_crc);
            }

            _fs3ImgInfo.tocArr[section_index].entry_addr = entry_addr;
            _fs3ImgInfo.tocArr[section_index].toc_entry = toc_entry;
            memcpy(_fs3ImgInfo.tocArr[section_index].data, entry_buffer, CIBFW_ITOC_ENTRY_SIZE);
        }
        section_index++;
    } while (toc_entry.type != FS3_END);
    _fs3ImgInfo.numOfItocs = section_index - 1;

    if (!mfg_exists) {
        _badDevDataSections = true;
        return errmsg(MLXFW_NO_MFG_ERR, "No \""MFG_INFO"\" info section.");
    }
    return ret_val;
}


bool Fs3Operations::FwVerify(VerifyCallBack verifyCallBackFunc, bool isStripedImage, bool showItoc) {
    //dummy assignment to avoid compiler warrning (isStripedImage is not used in fs3)
    (void)isStripedImage;

    struct QueryOptions queryOptions;
    queryOptions.readRom = true;
    queryOptions.quickQuery = false;

    return Fs3Verify(verifyCallBackFunc, showItoc, queryOptions);
}

#define BOOT_RECORD_SIZE 0x10
bool Fs3Operations::checkPreboot(u_int32_t* prebootBuff, u_int32_t size, VerifyCallBack verifyCallBackFunc)
{
    u_int32_t expectedCRC;
    char outputLine[512] = {0};
    u_int32_t startAddr = (_ioAccess->is_flash()) ? \
            _ioAccess->get_phys_from_cont(0x0, _fwImgInfo.cntxLog2ChunkSize, (_fwImgInfo.imgStart != 0)) : 0x0;

    sprintf(outputLine,"%s /0x%08x-0x%08x (0x%06x)/ (PREBOOT)", PRE_CRC_OUTPUT, startAddr, 0x34, size << 2);
    expectedCRC = prebootBuff[size-1];
    // calc CRC
    Crc16        crc1, crc2;
    CRC1n(crc1, prebootBuff, size);
    crc1.finish();
    // HACK: due to a bug in imgen this crc might not be calculated correctly(calculate in the "wrong way" for backward compat)
    // crc1 represents the proper way to calculate the crc , crc2 represents the "wrong" way

    // signature
    CRCn(crc2, prebootBuff, 4);
    // boot record
    u_int8_t bootRecordBE[BOOT_RECORD_SIZE];
    memcpy(bootRecordBE, &prebootBuff[4], BOOT_RECORD_SIZE);
    TOCPUn(bootRecordBE, (BOOT_RECORD_SIZE  >> 2));
    for (int i = 0; i < BOOT_RECORD_SIZE; i++) {
        crc2 << bootRecordBE[i];
    }
    // the rest of the section (leave last dword out of the crc calc as its the expected crc)
    CRC1n(crc2, &prebootBuff[8], size - 8);
    crc2.finish();

    // print results
    if (expectedCRC != crc1.get() && expectedCRC != crc2.get()) {
        report_callback(verifyCallBackFunc, "%s /0x%08x/ - wrong CRC (exp:0x%x, act:0x%x)\n",
                outputLine, startAddr, expectedCRC, crc1.get());
        return errmsg("Bad CRC");
    }
    report_callback(verifyCallBackFunc, "%s - OK\n", outputLine);
    return true;
}

bool Fs3Operations::CheckBinVersion(u_int8_t binVerMajor, u_int8_t binVerMinor)
{
    if (binVerMajor == 0 && binVerMinor == 0) {
        return true;
    }

    if (binVerMajor > FS3_MAX_BIN_VER_MAJOR) {
        return errmsg(MLXFW_UNSUPPORTED_BIN_VER_ERR, "Unsupported binary version (%d.%d) please update to latest MFT package", binVerMajor, binVerMinor);
    }

    if (binVerMajor < FS3_MIN_BIN_VER_MAJOR || (binVerMajor == FS3_MIN_BIN_VER_MAJOR && binVerMinor < FS3_MIN_BIN_VER_MINOR)) {
        return errmsg(MLXFW_UNSUPPORTED_BIN_VER_ERR, "Unsupported binary version (%d.%d) minimal supported version (%d.%d)", \
                binVerMajor, binVerMinor, FS3_MIN_BIN_VER_MAJOR, FS3_MIN_BIN_VER_MINOR);
    }

    return true;
}

bool Fs3Operations::Fs3Verify(VerifyCallBack verifyCallBackFunc, bool show_itoc, struct QueryOptions queryOptions)
{
    u_int32_t cntx_image_start[CNTX_START_POS_SIZE];
    u_int32_t cntx_image_num;
    u_int32_t buff[FS3_BOOT_START_IN_DW];
    u_int32_t offset;
    u_int8_t binVerMajor = 0, binVerMinor = 0;
    bool bad_signature;

    CntxFindAllImageStart(_ioAccess, cntx_image_start, &cntx_image_num);
    if (cntx_image_num == 0) {
        return errmsg(MLXFW_NO_VALID_IMAGE_ERR, "No valid FS3 image found");
    }
    if (cntx_image_num > 1) { // ATM we support only one valid image
        return errmsg(MLXFW_MULTIPLE_VALID_IMAGES_ERR, "More than one FS3 image found on %s", this->_ioAccess->is_flash() ? "Device" : "image");
    }
    u_int32_t image_start = cntx_image_start[0];
    offset = 0;
    // Read BOOT
    _ioAccess->set_address_convertor(0, 0);
    READBUF((*_ioAccess), image_start, buff, FS3_BOOT_START, "Image header");
    Fs3UpdateImgCache((u_int8_t*)buff, 0, FS3_BOOT_START);
    TOCPUn(buff, FS3_BOOT_START_IN_DW);
    _maxImgLog2Size = EXTRACT(buff[FS3_LOG2_CHUNK_SIZE_DW_OFFSET],16,8) ? EXTRACT(buff[FS3_LOG2_CHUNK_SIZE_DW_OFFSET],16,8) : FS3_LOG_CHUNK_SIZE;
    binVerMajor = EXTRACT(buff[FS3_LOG2_CHUNK_SIZE_DW_OFFSET], 8, 8);
    binVerMinor = EXTRACT(buff[FS3_LOG2_CHUNK_SIZE_DW_OFFSET], 0, 8);
    // check if binary version is supported by the tool
    if (!CheckBinVersion(binVerMajor, binVerMinor)) {
        return false;
    }
    // Put info
    _fwImgInfo.imgStart = image_start;
    // read the chunk size from the image header
    _fwImgInfo.cntxLog2ChunkSize = _maxImgLog2Size;
    _fwImgInfo.ext_info.is_failsafe        = true;
    _fwImgInfo.actuallyFailsafe  = true;
    _fwImgInfo.magicPatternFound = 1;
    _ioAccess->set_address_convertor(_fwImgInfo.cntxLog2ChunkSize, _fwImgInfo.imgStart != 0);


    report_callback(verifyCallBackFunc, "\nFS3 failsafe image\n\n");
    // adrianc: we dont check Preboot section (or boot start) because of a variance in the calculation of the CRC

    // Get BOOT2 -Get Only bootSize if quickQuery == true else read and check CRC of boot2 section as well
    offset += FS3_BOOT_START;
    FS3_CHECKB2(0, offset, !queryOptions.quickQuery, PRE_CRC_OUTPUT, verifyCallBackFunc);

    offset += _fwImgInfo.bootSize;
    _fs3ImgInfo.firstItocIsEmpty = false;
    // printf("-D- image_start = %#x\n", image_start);
    // Go over the ITOC entries
    u_int32_t sector_size = (_ioAccess->is_flash()) ? _ioAccess->get_sector_size() : FS3_DEFAULT_SECTOR_SIZE;
    offset = (offset % sector_size == 0) ? offset : (offset + sector_size - offset % 0x1000);
    while (offset < _ioAccess->get_size())
    {
        if (VerifyTOC(offset, bad_signature, verifyCallBackFunc, show_itoc, queryOptions)) {
            return true;
        } else {
            if (!bad_signature) {
                return false;
            }
            _fs3ImgInfo.firstItocIsEmpty = true;

        }
        offset += sector_size;
    }
    return errmsg(MLXFW_NO_VALID_ITOC_ERR, "No valid ITOC was found.");
}


bool Fs3Operations::Fs3IntQuery(bool readRom, bool quickQuery)
{
    struct QueryOptions queryOptions;
    queryOptions.readRom = readRom;
    queryOptions.quickQuery = quickQuery;

    if (!Fs3Verify((VerifyCallBack)NULL, 0, queryOptions)) {
        return false;
    }
    // get chip type and device sw id, from device/image
    const u_int32_t* swId = (u_int32_t*)NULL;
    if (_ioAccess->is_flash()) {
        if (!getInfoFromHwDevid(_ioAccess->get_dev_id(), _fwImgInfo.ext_info.chip_type, &swId)) {
            return false;
        }
        _fwImgInfo.ext_info.dev_type = swId[0];
    }
    return true;
}

bool Fs3Operations::FwQuery(fw_info_t *fwInfo, bool readRom, bool isStripedImage)
{
    //isStripedImage flag is not needed in FS3 image format
    // Avoid warning - no striped image in FS3
    (void)isStripedImage;
    if (!Fs3IntQuery(readRom)) {
        return false;
    }
    //adrianc:  best effort to get chip_type for image on image since it can "theoretically" be used on more than one device , take the first one.
    if (!_ioAccess->is_flash()) {
        const u_int32_t* swId = (u_int32_t*)NULL;
        if (!getInfoFromHwDevid(_fwImgInfo.supportedHwId[0], _fwImgInfo.ext_info.chip_type, &swId)) {
            return false;
        }
        _fwImgInfo.ext_info.dev_type = swId[0];
    }

    memcpy(&(fwInfo->fw_info),  &(_fwImgInfo.ext_info),  sizeof(fw_info_com_t));
    memcpy(&(fwInfo->fs3_info), &(_fs3ImgInfo.ext_info), sizeof(fs3_info_t));
    fwInfo->fw_type = FIT_FS3;
    return true;
}

u_int8_t Fs3Operations::FwType()
{
    return FIT_FS3;
}


bool Fs3Operations::FwInit()
{
    FwInitCom();
    memset(&_fs3ImgInfo, 0, sizeof(_fs3ImgInfo));
    _fwImgInfo.fwType = FIT_FS3;
    return true;
}

bool Fs3Operations::DevDataHackCheck(struct cibfw_itoc_entry *devTocEntry)
{
    if (_fwImgInfo.ext_info.chip_type != CT_CONNECT_IB) {
        return false;
    }
    switch(devTocEntry->type) {
    case FS3_NV_DATA2 :
    case FS3_FW_NV_LOG :
    case FS3_NV_DATA0 :
        if (devTocEntry->device_data == 0 && devTocEntry->no_crc == 0 && devTocEntry->relative_addr == 1) {
            return true;
        }
        break;
    default :
        break;
    }
    return false;
}

#define GET_DIFFER_STR(flash_toc_entry, image_toc_entry) \
        (flash_toc_entry->device_data != image_toc_entry->device_data) ? "device_data" : \
                (flash_toc_entry->no_crc != image_toc_entry->no_crc)   ? "no_crc" : \
                        (flash_toc_entry->relative_addr != image_toc_entry->relative_addr) ? "relative_addr" : ""

bool Fs3Operations::UpdateDevDataITOC(u_int8_t *image_data, struct toc_info *image_toc_info_entry, struct toc_info *flash_toc_arr, int flash_toc_size)
{
    u_int8_t itoc_data[CIBFW_ITOC_ENTRY_SIZE];
    struct cibfw_itoc_entry *image_toc_entry = &image_toc_info_entry->toc_entry;

    for (int i = 0; i < flash_toc_size; i++) {
        struct toc_info *flash_toc_info = &flash_toc_arr[i];
        struct cibfw_itoc_entry *flash_toc_entry = &flash_toc_info->toc_entry;
        if (flash_toc_entry->type == image_toc_entry->type) {
            // sanity checks on itoc entry
            if ( (flash_toc_entry->device_data != image_toc_entry->device_data) || \
                 (flash_toc_entry->no_crc != image_toc_entry->no_crc) || \
                 (flash_toc_entry->relative_addr != image_toc_entry->relative_addr)) {
                /* HACK: adrianc:remove at MFT-4.1.0
                 * if we encounter NV_DATA0/NV_DATA1/FW_NV_LOG that are marked in the device as non dev data
                 * it means a corrupt binary was burnt on device. take those entries from the image
                 */
                if (DevDataHackCheck(flash_toc_entry)) {
                    // perform HACK : i.e keep itoc entry as is in the image
                    continue;
                } else {
                    return errmsg(MLXFW_DEVICE_IMAGE_MISSMATCH_ERR, "An inconsistency was found in %s section attributes. %s ITOC attribute differs",\
                            GetSectionNameByType(image_toc_entry->type), GET_DIFFER_STR(flash_toc_entry, image_toc_entry));
                }
            }
            // replace itoc entry in the image
            memset(itoc_data, 0, CIBFW_ITOC_ENTRY_SIZE);
            cibfw_itoc_entry_pack(flash_toc_entry, itoc_data);
            memcpy(&image_data[image_toc_info_entry->entry_addr], itoc_data, CIBFW_ITOC_ENTRY_SIZE);
            cibfw_itoc_entry_unpack(&image_toc_info_entry->toc_entry, &image_data[image_toc_info_entry->entry_addr]);
        }
    }
    return true;
}

// add an itoc entry to the image  (just the entry no the section itself)
bool Fs3Operations::AddDevDataITOC(struct toc_info *flash_toc_entry, u_int8_t *image_data, struct toc_info *image_toc_arr, int& image_toc_size)
{

    if (image_toc_size + 1 > MAX_TOCS_NUM) {
        return errmsg("Cannot add iTOC entry, too many entries in iTOC array.");
    }
    if (!flash_toc_entry->toc_entry.device_data) {
        return errmsg("Cannot add non device data iTOC entry");
    }
    // add new entry to array
    image_toc_arr[image_toc_size].entry_addr = image_toc_arr[image_toc_size - 1].entry_addr + CIBFW_ITOC_ENTRY_SIZE;
    memcpy(image_toc_arr[image_toc_size].data, flash_toc_entry->data, CIBFW_ITOC_ENTRY_SIZE);
    image_toc_arr[image_toc_size].section_data = flash_toc_entry->section_data;
    image_toc_arr[image_toc_size].toc_entry = flash_toc_entry->toc_entry;
    // write entry data to image_data
    memcpy(&image_data[image_toc_arr[image_toc_size].entry_addr], image_toc_arr[image_toc_size].data, CIBFW_ITOC_ENTRY_SIZE);
    // write END itoc entry at the end of the array
    memset(&image_data[image_toc_arr[image_toc_size].entry_addr + CIBFW_ITOC_ENTRY_SIZE], 0xff, CIBFW_ITOC_ENTRY_SIZE);
    image_toc_size++;
    return true;
}

bool Fs3Operations::CheckFs3ImgSize(Fs3Operations& imageOps, bool useImageDevData)
{
    /* there are (ATM) two image slots on the flash:
     * SLOT0: starts add flash address 0x0
     * SLOT1: starts at flash address 2^_maxImgLogSize
     * Device sections can either be a part of SLOT0 Image or SLOT1 or not, depending on flash size
     * if flash size is greater than 2^(_maxImgLogSize+1) than device sections and first/second image dont share same area
     * */
    Fs3Operations& ops = useImageDevData ? imageOps : *this;
    u_int32_t maxFsImgSize = 1 << ops._maxImgLog2Size;
    u_int32_t smallestAbsAddrSlot0 = maxFsImgSize;
    u_int32_t smallestAbsAddrSlot1 = 2*maxFsImgSize;
    u_int32_t maxImgDataSizeSlot0, maxImgDataSizeSlot1;

    // find smallest abs address in SLOT0
    for (int i = 0; i < ops._fs3ImgInfo.numOfItocs; i++) {
        struct cibfw_itoc_entry *toc_entry = &ops._fs3ImgInfo.tocArr[i].toc_entry;
        u_int32_t tocEntryFlashAddr = toc_entry->flash_addr << 2;
        if (toc_entry->device_data) {
            if (tocEntryFlashAddr > maxFsImgSize) {
                // address in SLOT1
                smallestAbsAddrSlot1 = smallestAbsAddrSlot1 > tocEntryFlashAddr ? tocEntryFlashAddr : smallestAbsAddrSlot1;
            } else {
                // address in SLOT0
                smallestAbsAddrSlot0 = smallestAbsAddrSlot0 > tocEntryFlashAddr ? tocEntryFlashAddr : smallestAbsAddrSlot0;
            }
        }
    }
    maxImgDataSizeSlot0 = smallestAbsAddrSlot0;
    maxImgDataSizeSlot1 = smallestAbsAddrSlot1 - maxFsImgSize;

    u_int32_t maxImgDataSize = maxImgDataSizeSlot0 < maxImgDataSizeSlot1 ? maxImgDataSizeSlot0 : maxImgDataSizeSlot1;
    if (imageOps._fs3ImgInfo.sizeOfImgData > maxImgDataSize) {
        return errmsg(MLXFW_IMAGE_TOO_LARGE_ERR, "Size of image data (0x%x) is greater than max size of image data (0x%x)",
                imageOps._fs3ImgInfo.sizeOfImgData,  maxImgDataSize);
    }
    return true;
}

bool Fs3Operations::GetMaxImageSize(u_int32_t flash_size, bool image_is_fs, u_int32_t &max_image_size)
{
	// max image size is calculated as the following :
	// for failsafe image :   flash_size/2 - 6*sector_size
	// for nonfailsafe image: flash_size - 6*sector_size
	// the 6*sector_size is for the last two sections on the flash (DEV_INFO and MFG_INFO) which are not part of the image burnt.

    u_int32_t sector_size = _ioAccess->get_sector_size();
    if (image_is_fs) {
        max_image_size = (flash_size / 2) - (6 * sector_size);
    } else {
        // For non FS image,
        max_image_size = flash_size - (6 * sector_size);
    }

return true;
}

#define SUPPORTS_ISFU(chip_type) \
    (chip_type == CT_CONNECT_IB || chip_type == CT_CONNECTX4 || chip_type == CT_CONNECTX4_LX)

#define FLASH_RESTORE(origFlashObj) \
        if (origFlashObj) {\
            _ioAccess->close();\
            delete _ioAccess;\
            _ioAccess = origFlashObj;\
            _fwParams.ignoreCacheRep = 0;\
        }

bool Fs3Operations::BurnFs3Image(Fs3Operations &imageOps,
                                  ExtBurnParams& burnParams)
{
    u_int8_t  is_curr_image_in_odd_chunks;
    u_int32_t new_image_start;
    u_int32_t total_img_size = 0;
    u_int32_t sector_size = (_ioAccess->is_flash()) ? _ioAccess->get_sector_size() : FS3_DEFAULT_SECTOR_SIZE;

    Flash    *f     = (Flash*)(this->_ioAccess);
    FImage   *fim   = (FImage*)(imageOps._ioAccess);
    u_int8_t *data8 = (u_int8_t *) fim->getBuf();

    if (_fwImgInfo.imgStart != 0 || (!burnParams.burnFailsafe && ((Flash*)_ioAccess)->get_ignore_cache_replacment())) {
        // if the burn is not failsafe and with -ocr, the image is burnt at 0x0
        is_curr_image_in_odd_chunks = 1;
        new_image_start = 0;
    } else {
        is_curr_image_in_odd_chunks = 0;
        new_image_start = (1 << imageOps._fwImgInfo.cntxLog2ChunkSize);
    }
    /*printf("-D- new_image_start = %#x, is_curr_image_in_odd_chunks = %#x\n", new_image_start, is_curr_image_in_odd_chunks);
    printf("-D- num_of_itocs = %d\n", image_info->num_of_itocs);
    */

    // take chunk size from image in case of a non failsafe burn (in any case they should be the same)
    f->set_address_convertor(imageOps._fwImgInfo.cntxLog2ChunkSize, !is_curr_image_in_odd_chunks);

    // check max image size
    bool useImageDevData =  !burnParams.burnFailsafe && burnParams.useImgDevData;
    if (!CheckFs3ImgSize(imageOps, useImageDevData)) {
        return false;
    }

    // update devDataTocsInImage
    for (int i = 0; i < imageOps._fs3ImgInfo.numOfItocs; i++) {
        struct toc_info *itoc_info_p = &imageOps._fs3ImgInfo.tocArr[i];
        struct cibfw_itoc_entry *toc_entry = &itoc_info_p->toc_entry;

        if (toc_entry->device_data) {// updare dev_data itoc with the device's dev_data section addr
            if (burnParams.burnFailsafe || !burnParams.useImgDevData) {
                // we update the device data entires if : a. we burn failsafe or b. we burn non-failsafe but we take the device data anyway
                if(!UpdateDevDataITOC(data8, itoc_info_p, _fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs)){
                    return false;
                }
            }
        }
    }
    // sanity check on the image itoc array
    if (!imageOps.CheckItocArray()) {
        return errmsg(MLXFW_IMAGE_CORRUPTED_ERR, "%s", imageOps.err());
    }
    //find total image size that will be written
    for (int i = 0; i < imageOps._fs3ImgInfo.numOfItocs; i++) {
        struct toc_info *itoc_info_p = &imageOps._fs3ImgInfo.tocArr[i];
        struct cibfw_itoc_entry *toc_entry = &itoc_info_p->toc_entry;
        if (!toc_entry->device_data) {
            total_img_size += toc_entry->size << 2;
        } else if (!burnParams.burnFailsafe && burnParams.useImgDevData) {
            total_img_size += toc_entry->size << 2;
        } else {
            continue;
        }
    }
    // add boot section, itoc array (wo signature)
    total_img_size += imageOps._fs3ImgInfo.itocAddr + sector_size - FS3_FW_SIGNATURE_SIZE;

    if ( total_img_size <= sector_size) {
        return errmsg("Failed to burn FW. Internal error.");
    }

    u_int32_t  zeroes     = 0;
    // write the image
    int alreadyWrittenSz=0;
    // write boot section, itoc array (wo signature)
    if (!writeImage(burnParams.progressFunc,
                    FS3_FW_SIGNATURE_SIZE,
                    data8 + FS3_FW_SIGNATURE_SIZE,
                    imageOps._fs3ImgInfo.itocAddr + sector_size - FS3_FW_SIGNATURE_SIZE,
                    false,
                    total_img_size,
                    alreadyWrittenSz)) {
        return false;
    }
    alreadyWrittenSz += imageOps._fs3ImgInfo.itocAddr + sector_size - FS3_FW_SIGNATURE_SIZE;
    // write itoc entries data
    for (int i = 0; i < imageOps._fs3ImgInfo.numOfItocs; i++) {
        struct toc_info *itoc_info_p = &imageOps._fs3ImgInfo.tocArr[i];
        struct cibfw_itoc_entry *toc_entry = &itoc_info_p->toc_entry;
        bool writeSection = true;
        if (toc_entry->device_data && (burnParams.burnFailsafe || !burnParams.useImgDevData)) {
            writeSection = false;
        }

        if (writeSection) {
            if (!writeImage(burnParams.progressFunc, toc_entry->flash_addr << 2 , &(itoc_info_p->section_data[0]), itoc_info_p->section_data.size(), !toc_entry->relative_addr, total_img_size, alreadyWrittenSz)) {
                return false;
            }
            alreadyWrittenSz += itoc_info_p->section_data.size();
        }
    }

    if (!f->is_flash()) {
        return true;
    }
    // Write new signature
    if (!f->write(0, data8, 16, true)) {
        return false;
    }
    bool boot_address_was_updated = true;

    // if we access without cache replacement or the burn was non failsafe, update YU bootloaders.
    // if we access with cache replacement notify currently running fw of new image start address to crspace (for SW reset)
    //TODO: add SwitchIB, Spectrum when we have support for ISFU
    if (!SUPPORTS_ISFU(_fwImgInfo.ext_info.chip_type) || !burnParams.burnFailsafe || f->get_ignore_cache_replacment()) {
        boot_address_was_updated = f->update_boot_addr(new_image_start);
    } else {
        _isfuSupported = Fs3IsfuActivateImage(new_image_start);
        boot_address_was_updated = _isfuSupported;
    }

    if (imageOps._fwImgInfo.ext_info.is_failsafe) {
        if (!burnParams.burnFailsafe) {
            // When burning in nofs, remnant of older image with different chunk size
            // may reside on the flash -
            // Invalidate all images marking on flash except the one we've just burnt

            u_int32_t cntx_image_start[CNTX_START_POS_SIZE];
            u_int32_t cntx_image_num;

            CntxFindAllImageStart(f, cntx_image_start, &cntx_image_num);
            // Address convertor is disabled now - use phys addresses
            for (u_int32_t i = 0; i < cntx_image_num; i++) {
                if (cntx_image_start[i] != new_image_start) {
                    if (!f->write(cntx_image_start[i], &zeroes, sizeof(zeroes), true)) {
                        return errmsg(MLXFW_FLASH_WRITE_ERR, "Failed to invalidate old fw signature: %s", f->err());
                    }
                }
            }
        } else {
            // invalidate previous signature
            f->set_address_convertor(imageOps._fwImgInfo.cntxLog2ChunkSize, is_curr_image_in_odd_chunks);
            if (!f->write(0, &zeroes, sizeof(zeroes), true)) {
                return errmsg(MLXFW_FLASH_WRITE_ERR, "Failed to invalidate old fw signature: %s", f->err());
            }
        }
    }
    if (boot_address_was_updated == false) {
        report_warn("Failed to update FW boot address. Power cycle the device in order to load the new FW.\n");
    }
    return true;
}
bool Fs3Operations::Fs3Burn(Fs3Operations &imageOps, ExtBurnParams& burnParams)
{
    if (imageOps.FwType() != FIT_FS3) {
        return errmsg(MLXFW_IMAGE_FORMAT_ERR, "FW image type is not FS3\n");
    }
    bool devIntQueryRes = Fs3IntQuery();

    if (!devIntQueryRes && burnParams.burnFailsafe) {
        return false;
    }

    // for image we execute full verify to bring all the information needed for ROM Patch
    if (!imageOps.Fs3IntQuery(true, false)) {
          return false;
    }
    // Check Matching device ID
    if (!burnParams.noDevidCheck && _ioAccess->is_flash()) {
        if (imageOps._fwImgInfo.supportedHwIdNum) {
             if (!CheckMatchingHwDevId(_ioAccess->get_dev_id(),
                                         _ioAccess->get_rev_id(),
                                         imageOps._fwImgInfo.supportedHwId,
                                         imageOps._fwImgInfo.supportedHwIdNum)) {
                 return errmsg(MLXFW_DEVICE_IMAGE_MISSMATCH_ERR, "Device/Image mismatch: %s\n",this->err( ));
             }
         } else { // no suppored hw ids (problem with the image ?)
             return errmsg(MLXFW_DEVICE_IMAGE_MISSMATCH_ERR, "No supported devices were found in the FW image.");
         }
    }

    if (!burnParams.burnFailsafe) {
        // some checks in case we burn in a non-failsafe manner and attempt to integrate existing device
        // data sections from device.
        if (!burnParams.useImgDevData) { // we will take device data section from device: perform some checks
            if (_fs3ImgInfo.itocAddr == 0) {
                return errmsg("Cannot extract device data sections: invalid ITOC section. please ignore extracting device data sections.");
            }
            if (_badDevDataSections) {
                return errmsg("Cannot integrate device data sections: device data sections are corrupted. please ignore extracting device data sections.");
            }
        } else { // we will take device data sections from image: make sure device is not write protected
            if (_ioAccess->is_flash()) {
                FBase* origFlashObj = (FBase*)NULL;
                if (!((Flash*)_ioAccess)->get_ignore_cache_replacment()) {
                   origFlashObj = _ioAccess;
                   _fwParams.ignoreCacheRep = 1;
                   if (!FwOperations::FwAccessCreate(_fwParams, &_ioAccess)) {
                       _ioAccess = origFlashObj;
                       _fwParams.ignoreCacheRep = 0;
                       return errmsg(MLXFW_OPEN_OCR_ERR, "Failed to open device for direct flash access");
                   }
                }

                if (((Flash*)_ioAccess)->is_flash_write_protected()) {
                   FLASH_RESTORE(origFlashObj);
                   return errmsg("Cannot burn device data sections, Flash is write protected.");
                }
                FLASH_RESTORE(origFlashObj);
            }
        }
    }

    Fs3Operations *imgToBurn = &imageOps;
    bool createNewImg = false;
    std::vector<u_int8_t> newImageData(imageOps._fwImgInfo.lastImageAddr);

    if (devIntQueryRes && !CheckPSID(imageOps, burnParams.allowPsidChange)) {
        return false;
    }

    // no access to flash parameters under uefi
#ifndef UEFI_BUILD
    // TODO: adrianc: remove this in MFT-4.1.0
    if(burnParams.burnFailsafe) {
        if(!CheckAndFixCX4(false)) {
            return false;
        }
        if(!FixCX4WriteProtection(false)) {
            return false;
        }
    }
#endif

    if (burnParams.burnFailsafe) {
        // Check image and device chunk sizes are Ok
        if (_fwImgInfo.cntxLog2ChunkSize != imageOps._fwImgInfo.cntxLog2ChunkSize) {
            //TODO: remove both cond before release
            if (_ioAccess->get_size() < 0x1000000) {
                return errmsg(MLXFW_DEVICE_IMAGE_MISSMATCH_ERR, "Image intended for 16MB Flash. please replace your HCA or perform rework before updating.");
            }
            // if (imageOps._fwImgInfo.cntxLog2ChunkSize == 0x16 ) {
                return errmsg(MLXFW_FS_INFO_MISSMATCH_ERR, "Device and Image chunk sizes differ\n"
                              "    Internal users: in order to fix, please run:  mlxfwup -d %s -f %d.%04d.%04d --fix_cx4_image\n"
                              "    External users: contact Mellanox support team.", _devName, imageOps._fwImgInfo.ext_info.fw_ver[0],
                              imageOps._fwImgInfo.ext_info.fw_ver[1], imageOps._fwImgInfo.ext_info.fw_ver[2]); 
            //}
            // return errmsg("Device and Image chunk sizes differ, use non failsafe burn flow.");
        }

        // Check if the burnt FW version is OK
        if (!CheckFwVersion(imageOps, burnParams.ignoreVersionCheck)) {
            return false;
        }

        // ROM patchs
        if (((burnParams.burnRomOptions == ExtBurnParams::BRO_FROM_DEV_IF_EXIST) && (_fwImgInfo.ext_info.roms_info.exp_rom_found)) || // There is ROM in device and user choses to keep it
                ((burnParams.burnRomOptions == ExtBurnParams::BRO_DEFAULT) && (!imageOps._fwImgInfo.ext_info.roms_info.exp_rom_found && _fwImgInfo.ext_info.roms_info.exp_rom_found))) { // No ROM in image and ROM in device
            // here we should take rom from device and insert into the image
            // i.e if we have rom in image remove it and put the rom from the device else just put rom from device.
            // 1. use Fs3ModifySection to integrate _romSect buff with the image , newImageData contains the modified image buffer
            std::vector<u_int8_t> romSect = _romSect;
            TOCPUn((u_int32_t*)&romSect[0], romSect.size()/4);
            if (!imageOps.Fs3ReplaceSectionInDevImg(FS3_ROM_CODE, FS3_PCI_CODE, true, (u_int8_t*)&newImageData[0], imageOps._fwImgInfo.lastImageAddr,
                    (u_int32_t*)&romSect[0], (u_int32_t)romSect.size(), true)) {
                return errmsg(MLXFW_ROM_UPDATE_IN_IMAGE_ERR, "failed to update ROM in image. %s", imageOps.err());
            }
            createNewImg = true;
        }

        // image vsd patch
        if (!burnParams.useImagePs && burnParams.vsdSpecified ) {
            // get image info section :
            struct toc_info *imageInfoToc = NULL;
            if (!imageOps.Fs3GetItocInfo(imageOps._fs3ImgInfo.tocArr, imageOps._fs3ImgInfo.numOfItocs, FS3_IMAGE_INFO, imageInfoToc)){
                return errmsg(MLXFW_GET_SECT_ERR, "failed to get Image Info section.");
            }
            // modify it:
            std::vector<u_int8_t> imageInfoSect = imageInfoToc->section_data;
            struct cibfw_image_info image_info;
            cibfw_image_info_unpack(&image_info, &imageInfoSect[0]);
            strncpy(image_info.vsd, burnParams.userVsd, VSD_LEN);
            cibfw_image_info_pack(&image_info, &imageInfoSect[0]);
            // re-insert it into the image:
            if (!imageOps.Fs3ReplaceSectionInDevImg(FS3_IMAGE_INFO, FS3_FW_ADB, true, (u_int8_t*)&newImageData[0], imageOps._fwImgInfo.lastImageAddr,
                    (u_int32_t*)&imageInfoSect[0], (u_int32_t)imageInfoSect.size(), true)) {
                return errmsg(MLXFW_UPDATE_SECT_ERR, "failed to update image VSD in image. %s", imageOps.err());
            }
            createNewImg = true;
        }

        // create fs3Operation Obj (handl type BUFF) if NEEDED
        // open the image buffer
        if (createNewImg) {
            FwOperations* newImageOps = FwOperationsCreate((void*)&newImageData[0], (void*)&imageOps._fwImgInfo.lastImageAddr, (char*)NULL, FHT_FW_BUFF);
            if (!newImageOps) {
                return errmsg(MLXFW_IMAGE_CORRUPTED_ERR, "Internal error: The prepared image is corrupted.");
            }
            // 3. verify it
            if (!((Fs3Operations*)newImageOps)->Fs3IntQuery(true,false)) {
                errmsg(MLXFW_IMAGE_CORRUPTED_ERR, "Internal error: The prepared image is corrupted: %s", newImageOps->err());
                newImageOps->FwCleanUp();
                delete newImageOps;
                return false;
            }
            // 4. pass it to BurnFs3Image instead of imageOps
            imgToBurn = (Fs3Operations*)newImageOps;
        }
    }
    bool rc = BurnFs3Image(*imgToBurn, burnParams);
    if (imgToBurn != &imageOps) {
        imgToBurn->FwCleanUp();
        delete imgToBurn;
    }

    return rc ;
}

bool Fs3Operations::FwBurn(FwOperations *imageOps, u_int8_t forceVersion, ProgressCallBack progressFunc)
{
    if (imageOps == NULL) {
        return errmsg("bad parameter is given to FwBurn\n");
    }

    ExtBurnParams burnParams = ExtBurnParams();
    burnParams.ignoreVersionCheck = forceVersion;
    burnParams.progressFunc = progressFunc;

    return Fs3Burn((*(Fs3Operations*)imageOps), burnParams);
}

bool Fs3Operations::FwBurnAdvanced(FwOperations *imageOps, ExtBurnParams& burnParams)
{
    if (imageOps == NULL) {
        return errmsg("bad parameter is given to FwBurnAdvanced\n");
    }
    return Fs3Burn((*(Fs3Operations*)imageOps), burnParams);
}

bool Fs3Operations::FwBurnBlock(FwOperations *imageOps, ProgressCallBack progressFunc)
{
    // Avoid Warning!
    (void)imageOps;
    (void)progressFunc;
    return errmsg("FwBurnBlock is not supported anymore in FS3 image.");
}

bool Fs3Operations::FwReadData(void* image, u_int32_t* imageSize)
{
    struct QueryOptions queryOptions;
    if (!imageSize) {
        return errmsg("bad parameter is given to FwReadData\n");
    }

    queryOptions.readRom = true;
    queryOptions.quickQuery = false;
    if (image == NULL) {
        // When we need only to get size, no need for reading entire image
        queryOptions.readRom = false;
        queryOptions.quickQuery = true;
    }
    // Avoid Warning
    if (!Fs3Verify((VerifyCallBack)NULL, 0, queryOptions)) {
        return false;
    }

    if (image != NULL) {
        memcpy(image,  &_fs3ImgInfo.imageCache[0], _fwImgInfo.lastImageAddr);
    }
    *imageSize = _fwImgInfo.lastImageAddr;
    return true;
}

bool Fs3Operations::FwReadRom(std::vector<u_int8_t>& romSect)
{
    if (!Fs3IntQuery()) {
        return false;
    }
    if (_romSect.empty()) {
        return errmsg("Read ROM failed: The FW does not contain a ROM section");
    }
    romSect = _romSect;
    // Set endianness
    TOCPUn(&(romSect[0]), romSect.size()/4);
    return true;
}

bool Fs3Operations::FwGetSection (u_int32_t sectType, std::vector<u_int8_t>& sectInfo, bool stripedImage)
{
    (void) stripedImage; // unused for FS3
    //FwGetSection only supports retrieving FS3_DBG_FW_INI section atm.
    if (sectType != FS3_DBG_FW_INI) {
        return errmsg("Unsupported section type.");
    }
    //set the sector to read (need to remove it after read)
    _readSectList.push_back(sectType);
    if (!Fs3IntQuery()) {
        _readSectList.pop_back();
        return false;
    }
    _readSectList.pop_back();
    sectInfo = _fwConfSect;
    if (sectInfo.empty()) {
        return errmsg("Hash File section not found in the given image.");
    }
    return true;
}

bool Fs3Operations::FwSetMFG(fs3_uid_t baseGuid, PrintCallBack callBackFunc)
{
    if (!baseGuid.base_guid_specified && !baseGuid.base_mac_specified) {
        return errmsg("base GUID/MAC were not specified.");
    }

    if (baseGuid.base_mac_specified && !CheckMac(baseGuid.base_mac)) {
        return errmsg("Bad MAC (" MAC_FORMAT ") given: %s. Please specify a valid MAC value", baseGuid.base_mac.h, baseGuid.base_mac.l, err());
    }
    if (!baseGuid.use_pp_attr) {
        baseGuid.num_of_guids_pp[0] = baseGuid.num_of_guids ? baseGuid.num_of_guids : DEFAULT_GUID_NUM;
        baseGuid.step_size_pp[0] = baseGuid.step_size ? baseGuid.step_size : DEFAULT_STEP;
        baseGuid.num_of_guids_pp[1] = baseGuid.num_of_guids ? baseGuid.num_of_guids : DEFAULT_GUID_NUM;
        baseGuid.step_size_pp[1] = baseGuid.step_size ? baseGuid.step_size : DEFAULT_STEP;
        baseGuid.use_pp_attr = 1;
    }
    FAIL_NO_OCR("set manufacture GUIDs/MACs");
    if (!Fs3UpdateSection(&baseGuid, FS3_MFG_INFO, false, CMD_SET_MFG_GUIDS, callBackFunc)) {
        return false;
    }
    // on image verify that image is OK after modification (we skip this on device for performance reasons)
    if (!_ioAccess->is_flash() && !Fs3IntQuery(false, false)) {
        return false;
    }
    return true;
}

bool Fs3Operations::FwSetMFG(guid_t baseGuid, PrintCallBack callBackFunc)
{
    // in FS3 default behavior when setting GUIDs / MFG is to assign ini default step size and number.
    fs3_uid_t bGuid = {baseGuid, 1, {0, 0}, 0, 0, 0, 1, 1, {DEFAULT_GUID_NUM, DEFAULT_STEP}, {DEFAULT_GUID_NUM, DEFAULT_STEP}};
    return FwSetMFG(bGuid, callBackFunc);
}

bool Fs3Operations::FwSetGuids(sg_params_t& sgParam, PrintCallBack callBackFunc, ProgressCallBack progressFunc)
{
    fs3_uid_t usrGuid;
    memset(&usrGuid, 0, sizeof(usrGuid));
    // Avoid Warning because there is no need for progressFunc
    (void)progressFunc;
    if (sgParam.userGuids.empty()) {
        return errmsg("Base GUID not found.");
    }
    //query device to get mfg info (for guids override en bit)
    if (!Fs3IntQuery(false)) {
        return false;
    }

    if (!_fs3ImgInfo.ext_info.guids_override_en) {
    	return errmsg("guids override is not set, cannot set device guids");
    }

    usrGuid.num_of_guids_pp[0] = sgParam.usePPAttr ? sgParam.numOfGUIDsPP[0] : sgParam.numOfGUIDs ? sgParam.numOfGUIDs : DEFAULT_GUID_NUM;
    usrGuid.step_size_pp[0] =  sgParam.usePPAttr ? sgParam.stepSizePP[0] : sgParam.stepSize ? sgParam.stepSize : DEFAULT_STEP;
    usrGuid.num_of_guids_pp[1] = sgParam.usePPAttr ? sgParam.numOfGUIDsPP[1] : sgParam.numOfGUIDs ? sgParam.numOfGUIDs : DEFAULT_GUID_NUM;
    usrGuid.step_size_pp[1] =  sgParam.usePPAttr ? sgParam.stepSizePP[1] : sgParam.stepSize ? sgParam.stepSize : DEFAULT_STEP;
    usrGuid.use_pp_attr = 1;

    usrGuid.base_guid_specified = false;
    usrGuid.base_mac_specified = false;
    usrGuid.set_mac_from_guid = false;

    if (sgParam.guidsSpecified || sgParam.uidsSpecified) {
        usrGuid.base_guid_specified = true;
        usrGuid.base_guid = sgParam.userGuids[0];
        usrGuid.set_mac_from_guid = sgParam.uidsSpecified ? true : false;
    }
    if (sgParam.macsSpecified) {
        // check base mac
        if (!CheckMac(sgParam.userGuids[1])) {
            return errmsg("Bad MAC (" MAC_FORMAT ") given: %s. Please specify a valid MAC value", sgParam.userGuids[1].h, sgParam.userGuids[1].l, err());
        }
        usrGuid.base_mac_specified = true;
        usrGuid.base_mac = sgParam.userGuids[1];
    }

    if (!usrGuid.base_guid_specified && !usrGuid.base_mac_specified) {
        return errmsg("base GUID/MAC were not specified.");
    }
    FAIL_NO_OCR("set GUIDs/MACs");
    if (!Fs3UpdateSection(&usrGuid, FS3_DEV_INFO, false, CMD_SET_GUIDS, callBackFunc)) {
        return false;
    }
    // on image verify that image is OK after modification (we skip this on device for performance reasons)
    if (!_ioAccess->is_flash() && !Fs3IntQuery(false, false)) {
        return false;
    }
    return true;
}

bool Fs3Operations::FwSetVPD(char* vpdFileStr, PrintCallBack callBackFunc)
{
    if (!vpdFileStr) {
        return errmsg("Please specify a valid vpd file.");
    }
    FAIL_NO_OCR("set VPD");

    if (!Fs3UpdateSection(vpdFileStr, FS3_VPD_R0, false, CMD_BURN_VPD, callBackFunc)) {
        return false;
    }
    // on image verify that image is OK after modification (we skip this on device for performance reasons)
    if (!_ioAccess->is_flash() && !Fs3IntQuery(false, false)) {
        return false;
    }
    return true;
}

bool Fs3Operations::GetModifiedSectionInfo(fs3_section_t sectionType, fs3_section_t nextSectionType, u_int32_t &newSectAddr,
        fs3_section_t &SectToPut, u_int32_t &oldSectSize)
{
    struct toc_info *curr_itoc = (struct toc_info *)NULL;
    if (Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, sectionType, curr_itoc) ||
        Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, nextSectionType, curr_itoc)) {
        newSectAddr = curr_itoc->toc_entry.flash_addr << 2;
        SectToPut = (curr_itoc->toc_entry.type == sectionType) ? sectionType :  nextSectionType;
        oldSectSize = curr_itoc->toc_entry.size * 4;
        return true;
    }
    return false;
}

bool Fs3Operations::ShiftItocAddrInEntry(struct toc_info *newItocInfo, struct toc_info *oldItocInfo, int shiftSize)
{
    u_int32_t currSectaddr;
    CopyItocInfo(newItocInfo, oldItocInfo);
    currSectaddr =  (newItocInfo->toc_entry.flash_addr << 2) + shiftSize;
    Fs3UpdateItocInfo(newItocInfo, currSectaddr);
    return true;
}
bool Fs3Operations::Fs3UpdateItocInfo(struct toc_info *newItocInfo, u_int32_t newSectAddr, fs3_section_t sectionType, u_int32_t* newSectData,
        u_int32_t NewSectSize)
{
    std::vector<u_int8_t>  newSecVect(NewSectSize);
    newItocInfo->toc_entry.type = sectionType;
    memcpy(&newSecVect[0], newSectData, NewSectSize);
    return Fs3UpdateItocInfo(newItocInfo, newSectAddr, NewSectSize / 4, newSecVect);
}


bool Fs3Operations::CopyItocInfo(struct toc_info *newTocInfo, struct toc_info *currToc)
{
    memcpy(newTocInfo->data, currToc->data, CIBFW_ITOC_ENTRY_SIZE);
    newTocInfo->entry_addr = currToc->entry_addr;
    newTocInfo->section_data = currToc->section_data;
    newTocInfo->toc_entry = currToc->toc_entry;
    return true;
}




bool Fs3Operations::UpdateItocAfterInsert(fs3_section_t sectionType, u_int32_t newSectAddr, fs3_section_t SectToPut, bool toAdd, u_int32_t* newSectData,
        u_int32_t removedOrNewSectSize, struct toc_info *tocArr, u_int32_t &numOfItocs)
{
    bool isReplacement = (sectionType == SectToPut) ? true : false;
    int shiftSize;

    if (toAdd) {
        if (isReplacement) {
            struct toc_info *curr_itoc = (struct toc_info *)NULL;
            u_int32_t sectSize;
            if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, sectionType, curr_itoc)) {
                return false;
            }
            sectSize = curr_itoc->toc_entry.size * 4;
            shiftSize = (removedOrNewSectSize > sectSize) ? removedOrNewSectSize - sectSize : 0;
        } else {
            shiftSize = removedOrNewSectSize;
        }
        if (shiftSize % FS3_DEFAULT_SECTOR_SIZE) {
            shiftSize += (FS3_DEFAULT_SECTOR_SIZE - shiftSize % FS3_DEFAULT_SECTOR_SIZE);
        }
    } else {
        shiftSize = 0;
        if (removedOrNewSectSize % FS3_DEFAULT_SECTOR_SIZE) {
            removedOrNewSectSize += (FS3_DEFAULT_SECTOR_SIZE - removedOrNewSectSize % FS3_DEFAULT_SECTOR_SIZE);
        }
        shiftSize -= removedOrNewSectSize;
    }
    numOfItocs = 0;
    for (int i = 0; i < _fs3ImgInfo.numOfItocs; i++) {
        struct toc_info *curr_itoc = &_fs3ImgInfo.tocArr[i];
        struct toc_info *newTocInfo = &tocArr[numOfItocs];
        u_int32_t currSectaddr =  curr_itoc->toc_entry.flash_addr << 2; // Put it in one place

        if (currSectaddr > newSectAddr) {
            if (!curr_itoc->toc_entry.relative_addr) {

                CopyItocInfo(newTocInfo, curr_itoc);
            } else {
                ShiftItocAddrInEntry(newTocInfo, curr_itoc, shiftSize);
            }
        } else if (currSectaddr == newSectAddr) {
            if (!toAdd) {
                continue;
            }
            CopyItocInfo(newTocInfo, curr_itoc);
            Fs3UpdateItocInfo(newTocInfo, newSectAddr, sectionType, newSectData, removedOrNewSectSize);

            if (!isReplacement) {
                // put next section
                newTocInfo = &tocArr[++numOfItocs];
                ShiftItocAddrInEntry(newTocInfo, curr_itoc, shiftSize);
            }
        } else {
             // just Copy the ITOC as is
            CopyItocInfo(newTocInfo, curr_itoc);
        }
        numOfItocs++;
    }
    return true;
}

bool Fs3Operations::UpdateImageAfterInsert(struct toc_info *tocArr, u_int32_t numOfItocs, u_int8_t* newImgData, u_int32_t newImageSize)
{

    // Copy data before itocAddr and ITOC header
    memcpy(newImgData, &_fs3ImgInfo.imageCache[0], _fs3ImgInfo.itocAddr);
    memcpy(&newImgData[_fs3ImgInfo.itocAddr], _fs3ImgInfo.itocHeader, CIBFW_ITOC_HEADER_SIZE);
    for (int i = 0; i < (int)numOfItocs; i++) {
        // Inits
        u_int32_t itocOffset = _fs3ImgInfo.itocAddr + CIBFW_ITOC_HEADER_SIZE + i * CIBFW_ITOC_ENTRY_SIZE;
        struct toc_info *currItoc = &tocArr[i];
        u_int8_t sectType = currItoc->toc_entry.type;
        u_int32_t sectAddr = currItoc->toc_entry.flash_addr << 2;
        u_int32_t sectSize = currItoc->toc_entry.size * 4;
        // Some checks
        if (sectAddr + sectSize > newImageSize) {
            return errmsg("Internal error: Size of modified image (0x%x) is longer than size of original image (0x%x)!", sectAddr + sectSize, newImageSize);
        }
        if (sectSize != currItoc->section_data.size()) {
            return errmsg("Internal error: Sectoion size of %s (0x%x) is not equal to allocated memory for it(0x%x)", GetSectionNameByType(sectType),
                    sectSize, (u_int32_t)currItoc->section_data.size());
        }

        memcpy(&newImgData[itocOffset], currItoc->data, CIBFW_ITOC_ENTRY_SIZE);

        memcpy(&newImgData[sectAddr], &currItoc->section_data[0], sectSize);
    }
    u_int32_t lastItocSect = _fs3ImgInfo.itocAddr + CIBFW_ITOC_HEADER_SIZE + numOfItocs * CIBFW_ITOC_ENTRY_SIZE;
    memset(&newImgData[lastItocSect], FS3_END, CIBFW_ITOC_ENTRY_SIZE);
    return true;
}

bool Fs3Operations::Fs3ReplaceSectionInDevImg(fs3_section_t sectionType, fs3_section_t nextSectionType, bool toAdd, u_int8_t* newImgData, u_int32_t newImageSize,
        u_int32_t* newSectData, u_int32_t NewSectSize, bool UpdateExsistingTocArr)
{
    u_int32_t newSectAddr;
    u_int32_t numOfItocs;
    struct toc_info tocArr[MAX_TOCS_NUM];
    fs3_section_t sectToPut;
    u_int32_t oldSectSize;

    if (!GetModifiedSectionInfo(sectionType, nextSectionType, newSectAddr, sectToPut, oldSectSize)) {
        return false;
    }
    u_int32_t removedOrNewSectSize = (toAdd) ? NewSectSize : oldSectSize;

    if (!UpdateItocAfterInsert(sectionType, newSectAddr, sectToPut, toAdd, newSectData, removedOrNewSectSize, tocArr, numOfItocs)) {
        return false;
    }
    if (!UpdateImageAfterInsert(tocArr, numOfItocs, newImgData, newImageSize)) {
        return false;
    }

    if (UpdateExsistingTocArr) {
    	_fs3ImgInfo.numOfItocs = numOfItocs;
    	for (u_int32_t i=0;i < numOfItocs;i++) {
    		_fs3ImgInfo.tocArr[i] = tocArr[i];
    	}
    }
    return true;
}

bool Fs3Operations::Fs3ModifySection(fs3_section_t sectionType, fs3_section_t neighbourSection, bool toAdd, u_int32_t* newSectData, u_int32_t newSectSize,
        ProgressCallBack progressFunc)
{
    // Get image data and ROM data and integrate ROM data into image data
    // Verify FW on device
    if (!FwVerify((VerifyCallBack)NULL)) {
        return errmsg("Verify FW burn on the device failed: %s", err());
    }

    std::vector<u_int8_t> newImageData(_fwImgInfo.lastImageAddr);
    // u_int8_t *newImageData = new u_int8_t[_fwImgInfo.lastImageAddr];

    if (!Fs3ReplaceSectionInDevImg(sectionType, neighbourSection, toAdd, (u_int8_t*)&newImageData[0], _fwImgInfo.lastImageAddr,
            newSectData, newSectSize)) {
        return false;
    }
    // Burn the new image into the device.
    if (!FwBurnData((u_int32_t*)&newImageData[0], _fwImgInfo.lastImageAddr, progressFunc)) {
        return false;
    }
    return true;
}

bool Fs3Operations::Fs3AddSection(fs3_section_t sectionType, fs3_section_t neighbourSection, u_int32_t* newSectData, u_int32_t newSectSize,
        ProgressCallBack progressFunc)
{
    // We need to add the new section before the neighbourSection
    return Fs3ModifySection(sectionType, neighbourSection, true, newSectData, newSectSize, progressFunc);
}

bool Fs3Operations::Fs3RemoveSection(fs3_section_t sectionType, ProgressCallBack progressFunc)
{
    return Fs3ModifySection(sectionType, sectionType, false, (u_int32_t*)NULL, 0, progressFunc);
}


bool Fs3Operations::FwBurnRom(FImage* romImg, bool ignoreProdIdCheck, bool ignoreDevidCheck,
        ProgressCallBack progressFunc)
{
    roms_info_t romsInfo;

    if (romImg == NULL) {
        return errmsg("Bad ROM image is given.");
    }

    if (romImg->getBufLength() == 0) {
        return errmsg("Bad ROM file: Empty file.");
    }
    if (!FwOperations::getRomsInfo(romImg, romsInfo)) {
            return errmsg("Failed to read given ROM.");
    }
    if (!Fs3IntQuery(false)) {
        return false;
    }

    if (!ignoreProdIdCheck && strcmp(_fwImgInfo.ext_info.product_ver, "") != 0) {
        return errmsg("The device FW contains common FW/ROM Product Version - The ROM cannot be updated separately.");
    }

    if (!ignoreDevidCheck && !FwOperations::checkMatchingExpRomDevId(_fwImgInfo.ext_info.dev_type, romsInfo)) {
        return errmsg("Image file ROM: FW is for device %d, but Exp-ROM is for device %d\n", _fwImgInfo.ext_info.dev_type,
                romsInfo.exp_rom_com_devid);
    }
    return Fs3AddSection(FS3_ROM_CODE, FS3_PCI_CODE, romImg->getBuf(), romImg->getBufLength(), progressFunc);
}

bool Fs3Operations::FwDeleteRom(bool ignoreProdIdCheck, ProgressCallBack progressFunc)
{
    //run int query to get product ver
    if (!Fs3IntQuery(true)) {
        return false;
    }

    if (_romSect.empty()) {
        return errmsg("The FW does not contain a ROM section");
    }

    if (!ignoreProdIdCheck && strcmp(_fwImgInfo.ext_info.product_ver, "") != 0) {
        return errmsg("The device FW contains common FW/ROM Product Version - The ROM cannot be updated separately.");
    }

    return Fs3RemoveSection(FS3_ROM_CODE, progressFunc);
}

bool Fs3Operations::Fs3GetItocInfo(struct toc_info *tocArr, int num_of_itocs, fs3_section_t sect_type, struct toc_info *&curr_toc)
{
    for (int i = 0; i < num_of_itocs; i++) {
        struct toc_info *itoc_info = &tocArr[i];
        if (itoc_info->toc_entry.type == sect_type) {
            curr_toc =  itoc_info;
            return true;
        }
    }
    return errmsg("ITOC entry type: %s (%d) not found", GetSectionNameByType(sect_type), sect_type);
}

bool Fs3Operations::Fs3UpdateMfgUidsSection(struct toc_info *curr_toc, std::vector<u_int8_t>  section_data, fs3_uid_t base_uid,
                                            std::vector<u_int8_t>  &newSectionData)
{
    struct cibfw_mfg_info cib_mfg_info;
    struct cx4fw_mfg_info cx4_mfg_info;
    cibfw_mfg_info_unpack(&cib_mfg_info, (u_int8_t*)&section_data[0]);

    if (CHECK_MFG_OLD_FORMAT(cib_mfg_info)) {
        if (!Fs3ChangeUidsFromBase(base_uid, cib_mfg_info.guids)) {
            return false;
        }
    } else if (CHECK_MFG_NEW_FORMAT(cib_mfg_info)) {
        cx4fw_mfg_info_unpack(&cx4_mfg_info, (u_int8_t*)&section_data[0]);
        if (!Fs3ChangeUidsFromBase(base_uid, cx4_mfg_info.guids)) {
            return false;
        }
    } else {
        return errmsg("Unknown MFG_INFO format version (%d.%d).", cib_mfg_info.major_version, cib_mfg_info.minor_version);
    }
    newSectionData = section_data;
    memset((u_int8_t*)&newSectionData[0], 0, curr_toc->toc_entry.size * 4);

    if (CHECK_MFG_NEW_FORMAT(cib_mfg_info)) {
        cx4fw_mfg_info_pack(&cx4_mfg_info, (u_int8_t*)&newSectionData[0]);
    } else {
        cibfw_mfg_info_pack(&cib_mfg_info, (u_int8_t*)&newSectionData[0]);
    }
    return true;
}

#define GUID_TO_64(guid_st) \
        (guid_st.l | (u_int64_t)guid_st.h << 32)

bool Fs3Operations::Fs3ChangeUidsFromBase(fs3_uid_t base_uid, struct cibfw_guids& guids)
{
    /*
     * On ConnectIB and SwitchIB we derrive macs and guids from a single base_guid
     */
    u_int64_t base_guid_64;
    u_int64_t base_mac_64;
    if (!base_uid.use_pp_attr) {
        return errmsg("Expected per port attributes to be specified");
    }

    base_guid_64 = GUID_TO_64(base_uid.base_guid);
    base_mac_64 = (((u_int64_t)base_uid.base_guid.l & 0xffffff) | (((u_int64_t)base_uid.base_guid.h & 0xffffff00) << 16));
    guids.guids[0].uid = base_guid_64;
    guids.guids[0].num_allocated = base_uid.num_of_guids_pp[0] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[0] : guids.guids[0].num_allocated;
    guids.guids[0].step = base_uid.step_size_pp[0] != DEFAULT_STEP ? base_uid.step_size_pp[0] : guids.guids[0].step;

    guids.guids[1].uid = base_guid_64 + (guids.guids[0].num_allocated * guids.guids[0].step);
    guids.guids[1].num_allocated = base_uid.num_of_guids_pp[1] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[1] : guids.guids[1].num_allocated;
    guids.guids[1].step = base_uid.step_size_pp[1] != DEFAULT_STEP ? base_uid.step_size_pp[1] : guids.guids[1].step;

    guids.macs[0].uid = base_mac_64;
    guids.macs[0].num_allocated = base_uid.num_of_guids_pp[0] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[0] : guids.macs[0].num_allocated;
    guids.macs[0].step = base_uid.step_size_pp[0] != DEFAULT_STEP ? base_uid.step_size_pp[0] : guids.macs[0].step;

    guids.macs[1].uid = base_mac_64 + (guids.macs[0].num_allocated * guids.macs[0].step);
    guids.macs[1].num_allocated = base_uid.num_of_guids_pp[1] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[1] : guids.macs[1].num_allocated;
    guids.macs[1].step = base_uid.step_size_pp[1] != DEFAULT_STEP ? base_uid.step_size_pp[1] : guids.macs[1].step;
    return true;
}

bool Fs3Operations::Fs3ChangeUidsFromBase(fs3_uid_t base_uid, struct cx4fw_guids& guids)
{
    /*
     * on ConnectX4 we derrive guids from base_guid and macs from base_mac
     */
    u_int64_t base_guid_64;
    u_int64_t base_mac_64;
    if (!base_uid.use_pp_attr) {
        return errmsg("Expected per port attributes to be specified");
    }

    base_guid_64 = base_uid.base_guid_specified ? GUID_TO_64(base_uid.base_guid) : guids.guids.uid;
    base_mac_64 = base_uid.base_mac_specified ? GUID_TO_64(base_uid.base_mac) : guids.macs.uid;
    if (base_uid.set_mac_from_guid && base_uid.base_guid_specified) {
        // in case we derrive mac from guid
        base_mac_64 = (((u_int64_t)base_uid.base_guid.l & 0xffffff) | (((u_int64_t)base_uid.base_guid.h & 0xffffff00) << 16));
    }

    guids.guids.uid = base_guid_64;
    guids.guids.num_allocated = base_uid.num_of_guids_pp[0] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[0] : guids.guids.num_allocated;
    guids.guids.step = base_uid.step_size_pp[0] != DEFAULT_STEP ? base_uid.step_size_pp[0] : guids.guids.step;

    guids.macs.uid = base_mac_64;
    guids.macs.num_allocated = base_uid.num_of_guids_pp[0] != DEFAULT_GUID_NUM ? base_uid.num_of_guids_pp[0] : guids.macs.num_allocated;
    guids.macs.step = base_uid.step_size_pp[0] != DEFAULT_STEP ? base_uid.step_size_pp[0] : guids.macs.step ;
    return true;
}

bool Fs3Operations::Fs3UpdateUidsSection(struct toc_info *curr_toc, std::vector<u_int8_t>  section_data, fs3_uid_t base_uid,
        std::vector<u_int8_t>  &newSectionData)
{
    struct cibfw_device_info cib_dev_info;
    struct cx4fw_device_info cx4_dev_info;
    cibfw_device_info_unpack(&cib_dev_info, (u_int8_t*)&section_data[0]);

    if (CHECK_DEV_INFO_OLD_FORMAT(cib_dev_info)) {
        if (!Fs3ChangeUidsFromBase(base_uid, cib_dev_info.guids)) {
            return false;
        }
    } else if (CHECK_DEV_INFO_NEW_FORMAT(cib_dev_info)) {
        cx4fw_device_info_unpack(&cx4_dev_info, (u_int8_t*)&section_data[0]);
        if (!Fs3ChangeUidsFromBase(base_uid, cx4_dev_info.guids)) {
            return false;
        }
    } else {
        return errmsg("Unknown DEV_INFO format version (%d.%d).", cib_dev_info.major_version, cib_dev_info.minor_version);
    }
    newSectionData = section_data;
    memset((u_int8_t*)&newSectionData[0], 0, curr_toc->toc_entry.size * 4);

    if (CHECK_DEV_INFO_NEW_FORMAT(cib_dev_info)) {
        cx4fw_device_info_pack(&cx4_dev_info, (u_int8_t*)&newSectionData[0]);
    } else {
        cibfw_device_info_pack(&cib_dev_info, (u_int8_t*)&newSectionData[0]);
    }
    return true;
}

bool Fs3Operations::Fs3UpdateVsdSection(struct toc_info *curr_toc, std::vector<u_int8_t>  section_data, char* user_vsd,
        std::vector<u_int8_t>  &newSectionData)
{
    struct cibfw_device_info dev_info;
    cibfw_device_info_unpack(&dev_info, (u_int8_t*)&section_data[0]);
    memset(dev_info.vsd, 0, sizeof(dev_info.vsd));
    strncpy(dev_info.vsd, user_vsd, TOOLS_ARR_SZ(dev_info.vsd) - 1);
    newSectionData = section_data;
    memset((u_int8_t*)&newSectionData[0], 0, curr_toc->toc_entry.size * 4);
    cibfw_device_info_pack(&dev_info, (u_int8_t*)&newSectionData[0]);
    return true;
}

bool Fs3Operations::Fs3UpdateVpdSection(struct toc_info *curr_toc, char *vpd,
                               std::vector<u_int8_t>  &newSectionData)
{
    int vpd_size = 0;
    u_int8_t *vpd_data = (u_int8_t*)NULL;

    if (!ReadImageFile(vpd, vpd_data, vpd_size)) {
        return false;
    }
    if (vpd_size % 4) {
        delete[] vpd_data;
        return errmsg("Size of VPD file: %d is not 4-byte alligned!", vpd_size);
    }
    GetSectData(newSectionData, (u_int32_t*)vpd_data, vpd_size);
    curr_toc->toc_entry.size = vpd_size / 4;
    delete[] vpd_data;
    return true;
}

// all device data section might be shifted by SHIFT_SIZE due to
// flash with write protect sector 0f 64kb instead of 4kb
#define SHIFT_SIZE 0xf000 // 60kb


bool Fs3Operations::Fs3GetNewSectionAddr(struct toc_info *curr_toc, u_int32_t &NewSectionAddr, bool failsafe_section)
{
    u_int32_t flash_addr = curr_toc->toc_entry.flash_addr << 2;

    // HACK: THIS IS AN UGLY HACK, SHOULD BE REMOVED ASAP
    // Possible solution : if a section is failsafe  make its size 2kb thus both section will fit in a 4kb chunk (addr & 0x800 == 0x800 then its in second place, if == 0 its in first place )

    if (failsafe_section) {// we assume dev_info is the only FS section.
        // get the two dev_info addresses (section is failsafe) according to the location of the mfg section
        toc_info* toc = NULL;
        u_int32_t devInfoAddr1 = 0;
        u_int32_t devInfoAddr2 = 0;

        if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, FS3_MFG_INFO, toc)) {
                 return errmsg("failed to locate MFG_INFO address within the FW image");
             }
        // calculate device info sections (fs section) address according to the MFG section
        // (i.e we assume they are located in: mfg_addr - 4k and mfg_addr - 8k)
        devInfoAddr1 = (toc->toc_entry.flash_addr << 2) - 0x1000;
        devInfoAddr2 = (toc->toc_entry.flash_addr << 2) - 0x2000;
        //printf("-D-mfg_section: 0x%x devInfoAddr1: 0x%x devInfoAddr2: 0x%x\n", toc->toc_entry.flash_addr, devInfoAddr1, devInfoAddr2);

        if ((flash_addr == devInfoAddr1) || (flash_addr == devInfoAddr2)){
            NewSectionAddr = (flash_addr == devInfoAddr1) ? devInfoAddr2 : devInfoAddr1;
        } else {
            // FW image is a mess
            return errmsg("DEV_INFO section is located in an unexpected address(0x%x)", flash_addr);
        }
    } else {
        NewSectionAddr = flash_addr;
    }
    return true;
}

bool Fs3Operations::CalcItocEntryCRC(struct toc_info *curr_toc)
{
    u_int8_t  new_entry_data[CIBFW_ITOC_ENTRY_SIZE];
    memset(new_entry_data, 0, CIBFW_ITOC_ENTRY_SIZE);

    cibfw_itoc_entry_pack(&curr_toc->toc_entry, new_entry_data);
    u_int32_t entry_crc = CalcImageCRC((u_int32_t*)new_entry_data, (TOC_ENTRY_SIZE / 4) - 1);
    curr_toc->toc_entry.itoc_entry_crc = entry_crc;
    return true;

}

bool Fs3Operations::Fs3UpdateItocData(struct toc_info *currToc)
{
    CalcItocEntryCRC(currToc);
    memset(currToc->data, 0, CIBFW_ITOC_ENTRY_SIZE);
    cibfw_itoc_entry_pack(&currToc->toc_entry, currToc->data);

    return true;

}

bool Fs3Operations::Fs3UpdateItocInfo(struct toc_info *curr_toc, u_int32_t newSectionAddr)
{
    // We assume it's absolute
     curr_toc->toc_entry.flash_addr = newSectionAddr >> 2;
     return Fs3UpdateItocData(curr_toc);

}
bool Fs3Operations::Fs3UpdateItocInfo(struct toc_info *curr_toc, u_int32_t newSectionAddr, u_int32_t NewSectSize, std::vector<u_int8_t>  newSectionData)
{
    curr_toc->section_data = newSectionData;
    curr_toc->toc_entry.size = NewSectSize;
    u_int32_t new_crc = CalcImageCRC((u_int32_t*)&newSectionData[0], curr_toc->toc_entry.size);
    curr_toc->toc_entry.section_crc = new_crc;

    return Fs3UpdateItocInfo(curr_toc, newSectionAddr);
 }

bool Fs3Operations::Fs3ReburnItocSection(u_int32_t newSectionAddr,
        u_int32_t newSectionSize, std::vector<u_int8_t>  newSectionData, const char *msg, PrintCallBack callBackFunc)
{
    char message[127];

    sprintf(message, "Updating %-4s section - ", msg);
    // Burn new Section
    // we pass a null callback and print the progress here as the writes are small (guids/mfg/vpd_str)
    // in the future if we want to pass the cb prints to writeImage , need to change the signature of progressCallBack to recieve and optional string to print

    PRINT_PROGRESS(callBackFunc, message);

    if (!writeImage((ProgressCallBack)NULL, newSectionAddr , (u_int8_t*)&newSectionData[0], newSectionSize, true)) {
    	PRINT_PROGRESS(callBackFunc, (char*)"FAILED\n");
        return false;
    }
    PRINT_PROGRESS(callBackFunc, (char*)"OK\n");
    // Update new ITOC section
    if (!reburnItocSection(callBackFunc)) {
    	return false;
    }
    return true;
}

//add callback if we want info during section update
bool  Fs3Operations::Fs3UpdateSection(void *new_info, fs3_section_t sect_type, bool is_sect_failsafe, CommandType cmd_type, PrintCallBack callBackFunc)
{
    struct toc_info *curr_toc = NULL;
    std::vector<u_int8_t> newUidSection;
    u_int32_t newSectionAddr;
    const char *type_msg;
    // init sector to read
    _readSectList.push_back(sect_type);
    if (!Fs3IntQuery()) {
        _readSectList.pop_back();
        return false;
    }
    _readSectList.pop_back();
    // _silent = curr_silent;

    if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, sect_type, curr_toc)) {
         return false;
     }

    if (sect_type == FS3_MFG_INFO) {
        fs3_uid_t base_uid = *(fs3_uid_t*)new_info;
        type_msg = "GUID";
        if (!Fs3UpdateMfgUidsSection(curr_toc, curr_toc->section_data, base_uid, newUidSection)) {
            return false;
        }
    } else if (sect_type == FS3_DEV_INFO) {
        if (cmd_type == CMD_SET_GUIDS) {
            fs3_uid_t base_uid = *(fs3_uid_t*)new_info;
            type_msg = "GUID";
            if (!Fs3UpdateUidsSection(curr_toc, curr_toc->section_data, base_uid, newUidSection)) {
                return false;
            }
        } else if(cmd_type == CMD_SET_VSD) {
            char* user_vsd = (char*)new_info;
            type_msg = "VSD";
            if (!Fs3UpdateVsdSection(curr_toc, curr_toc->section_data, user_vsd, newUidSection)) {
                return false;
            }
            } else {
                // We shouldnt reach here EVER
                type_msg = (char*)"Unknown";
        }
    } else if (sect_type == FS3_VPD_R0) {
        char *vpd_file = (char*)new_info;
        type_msg = "VPD";
        if (!Fs3UpdateVpdSection(curr_toc, vpd_file, newUidSection)) {
            return false;
        }
    } else {
        return errmsg("Section type %s is not supported\n", GetSectionNameByType(sect_type));
    }


    if (!Fs3GetNewSectionAddr(curr_toc, newSectionAddr, is_sect_failsafe)) {
        return false;
    }
    if (!Fs3UpdateItocInfo(curr_toc, newSectionAddr, curr_toc->toc_entry.size, newUidSection)) {
        return false;
    }
    if (!Fs3ReburnItocSection(newSectionAddr, curr_toc->toc_entry.size * 4, newUidSection, type_msg, callBackFunc)) {
        return false;
    }
    return true;
}


bool Fs3Operations::FwSetVSD(char* vsdStr, ProgressCallBack progressFunc, PrintCallBack printFunc)
{
    // Avoid warning
    (void)progressFunc;
    if (!vsdStr) {
        return errmsg("Please specify a valid VSD string.");
    }

    if (strlen(vsdStr) > VSD_LEN) {
    	return errmsg("VSD string is too long(%d), max allowed length: %d", (int)strlen(vsdStr), (int)VSD_LEN);
    }
    FAIL_NO_OCR("set VSD");
    if (!Fs3UpdateSection(vsdStr, FS3_DEV_INFO, false, CMD_SET_VSD, printFunc)) {
        return false;
    }
    // on image verify that image is OK after modification (we skip this on device for performance reasons)
    if (!_ioAccess->is_flash() && !Fs3IntQuery(false, false)) {
        return false;
    }
    return true;
}

bool Fs3Operations::FwSetAccessKey(hw_key_t userKey, ProgressCallBack progressFunc)
{
    (void)userKey;
    (void)progressFunc;
    return errmsg("Set access key not supported.");
}

bool Fs3Operations::FwResetNvData()
{
	return errmsg("Unsupported Device, can only reset configuration on a CX3/3-PRO device.");
	/*
	// future support for cx4

	if (!Fs3IntQuery(false)) {
		return false;
	}
	if (_fwImgInfo.ext_info.chip_type != CT_CONNECTX) {
		// TODO: Indicate the device name.
		   return errmsg("Unsupported device type %d", _fwImgInfo.ext_info.dev_type);
	}

	struct toc_info *currToc;

	if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, FS3_NV_DATA, currToc)) {
		return false;
	}
	// allocate new NvData which will contain only zeroes
	std::vector<u_int8_t> newNvData(currToc->section_data.size());
	memset(&newNvData[0], 0, currToc->section_data.size());

	return Fs3AddSection(FS3_NV_DATA, FS3_DEV_INFO, (u_int32_t*)&newNvData[0], newNvData.size()/4, progressFunc);
	*/
}

u_int32_t Fs3Operations::getAbsAddr(toc_info* toc) {
	if (toc->toc_entry.relative_addr) {
		return ((toc->toc_entry.flash_addr << 2) + _fwImgInfo.imgStart);
	}
	return toc->toc_entry.flash_addr << 2;
}

u_int32_t Fs3Operations::getAbsAddr(toc_info* toc, u_int32_t imgStart) {
    if (toc->toc_entry.relative_addr) {
        return ((toc->toc_entry.flash_addr << 2) + imgStart);
    }
    return toc->toc_entry.flash_addr << 2;
}

//get the last fw section address (i.e the maximal address + size of the fw section)
bool Fs3Operations::getLastFwSAddr(u_int32_t& lastAddr) {
	struct toc_info *maxToc= (struct toc_info*)NULL;
	int i;
	// find first itoc that isnt device data (assumption: there is at least one)
	for(i=0 ; i < _fs3ImgInfo.numOfItocs ; i++) {
		maxToc = &(_fs3ImgInfo.tocArr[i]);
		if (!maxToc->toc_entry.device_data) {
			break;
		}
	}
	// find the last non device data itoc
	for(; i < _fs3ImgInfo.numOfItocs ; i++) {
		if ((!_fs3ImgInfo.tocArr[i].toc_entry.device_data) && getAbsAddr(&(_fs3ImgInfo.tocArr[i])) > getAbsAddr(maxToc)) {
			maxToc = &_fs3ImgInfo.tocArr[i];
		}
	}
	lastAddr = getAbsAddr(maxToc) + (maxToc->toc_entry.size << 2 );
	return true;
}

bool Fs3Operations::getFirstDevDataAddr(u_int32_t& firstAddr) {
	struct toc_info *minToc = (struct toc_info*)NULL;
	//find first dev data itoc entry
	int i;
	for(i=0 ; i < _fs3ImgInfo.numOfItocs ; i++) {
		if (_fs3ImgInfo.tocArr[i].toc_entry.device_data) {
			minToc = &(_fs3ImgInfo.tocArr[i]);
			break;
		}
	}
	if (!minToc) {
		return errmsg("failed to get device data ITOC.");
	}
	i++;
	// find the minimal one
	for(; i < _fs3ImgInfo.numOfItocs ; i++) {
		if (_fs3ImgInfo.tocArr[i].toc_entry.device_data && (getAbsAddr(&(_fs3ImgInfo.tocArr[i])) < getAbsAddr(minToc)) ) {
			minToc = &(_fs3ImgInfo.tocArr[i]);
		}
	}
	firstAddr = getAbsAddr(minToc);
	return true;
}

bool Fs3Operations::reburnItocSection(PrintCallBack callBackFunc) {

    // HACK SHOULD BE REMOVED ASAP
    u_int32_t sector_size = (_ioAccess->is_flash()) ? _ioAccess->get_sector_size() : FS3_DEFAULT_SECTOR_SIZE;
    // Itoc section is failsafe (two sectors after boot section are reserved for itoc entries)
    u_int32_t oldItocAddr = _fs3ImgInfo.itocAddr;
    u_int32_t newItocAddr = (_fs3ImgInfo.firstItocIsEmpty) ? (_fs3ImgInfo.itocAddr - sector_size) :  (_fs3ImgInfo.itocAddr + sector_size);
    // Update new ITOC
    u_int32_t itocSize = (_fs3ImgInfo.numOfItocs + 1 ) * CIBFW_ITOC_ENTRY_SIZE + CIBFW_ITOC_HEADER_SIZE;
    u_int8_t *p = new u_int8_t[itocSize];
    memcpy(p, _fs3ImgInfo.itocHeader, CIBFW_ITOC_HEADER_SIZE);
    for (int i = 0; i < _fs3ImgInfo.numOfItocs; i++) {
        struct toc_info *curr_itoc = &_fs3ImgInfo.tocArr[i];
        memcpy(p + CIBFW_ITOC_HEADER_SIZE + i * CIBFW_ITOC_ENTRY_SIZE, curr_itoc->data, CIBFW_ITOC_ENTRY_SIZE);
    }
    memset(&p[itocSize] - CIBFW_ITOC_ENTRY_SIZE, FS3_END, CIBFW_ITOC_ENTRY_SIZE);

    PRINT_PROGRESS(callBackFunc, (char*)"Updating ITOC section - ");
    bool rc = writeImage((ProgressCallBack)NULL, newItocAddr , p, itocSize, false);
    delete[] p;
    if (!rc) {
    	PRINT_PROGRESS(callBackFunc,(char*)"FAILED\n");
        return false;
    }
    PRINT_PROGRESS(callBackFunc,(char*)"OK\n");
    u_int32_t zeros = 0;

    PRINT_PROGRESS(callBackFunc,(char*)"Restoring signature   - ");
    if (!writeImage((ProgressCallBack)NULL, oldItocAddr, (u_int8_t*)&zeros, 4, false)) {
        PRINT_PROGRESS(callBackFunc,(char*)"FAILED\n");
        return false;
    }
    PRINT_PROGRESS(callBackFunc,(char*)"OK\n");
    return true;
}

#define PUSH_DEV_DATA(vec)\
        vec.push_back(FS3_MFG_INFO);\
        vec.push_back(FS3_DEV_INFO);\
        vec.push_back(FS3_NV_DATA0);\
        vec.push_back(FS3_NV_DATA1);\
        vec.push_back(FS3_NV_DATA2);\
        vec.push_back(FS3_FW_NV_LOG);\
        vec.push_back(FS3_VPD_R0)
#define POP_DEV_DATA(vec)\
        vec.pop_back();\
        vec.pop_back();\
        vec.pop_back();\
        vec.pop_back();\
        vec.pop_back();\
        vec.pop_back();\
        vec.pop_back()

bool Fs3Operations::TocComp::operator() (toc_info* elem1, toc_info* elem2)
{
	u_int32_t absAddr1 = (elem1->toc_entry.flash_addr << 2) + ( elem1->toc_entry.relative_addr ? _startAdd : 0);
	u_int32_t absAddr2 = (elem2->toc_entry.flash_addr << 2) + ( elem2->toc_entry.relative_addr ? _startAdd : 0);
	if (absAddr1 < absAddr2) {
		return true;
	}
	return false;
}

bool Fs3Operations::FwShiftDevData(PrintCallBack progressFunc)
{
	if (!_ioAccess->is_flash()) {
		return errmsg("cannot shift device data sections on Image.");
	}
	const char* flashType = ((Flash*)_ioAccess)->getFlashType();
	if (flashType == NULL) {
		return errmsg("Cannot shift device data on old flash types.");
	}
	if (strcasecmp(flashType,"N25Q0XX")!= 0) {
		return errmsg("Cannot shift device data on flash type %s.", flashType);
	}

	//query device and get device data sectors.
	PUSH_DEV_DATA(_readSectList);
    if (!Fs3IntQuery()) {
    	POP_DEV_DATA(_readSectList);
        return false;
    }
    POP_DEV_DATA(_readSectList);

    if (_fwImgInfo.ext_info.chip_type != CT_CONNECT_IB) {
        return errmsg("Cannot shift device data. Unsupported device.");
    }

	u_int32_t lastFwDataAddr;
	u_int32_t firstDevDataAddr;
	if (!getLastFwSAddr(lastFwDataAddr) || !getFirstDevDataAddr(firstDevDataAddr)) {
		return errmsg("Failed to get ITOC information.");
	}

	// check if we already shifted
	struct toc_info* mfgToc = (struct toc_info*)NULL;
	if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, FS3_MFG_INFO, mfgToc)) {
		return errmsg("Failed to get MFG_INFO ITOC information.");
	}

	if (getAbsAddr(mfgToc) < _ioAccess->get_size() - _ioAccess->get_sector_size()) {
		return errmsg("Device data sections already shifted.");
	}

	//check if we can shift all dev data sections by 60KB
	if (lastFwDataAddr > (firstDevDataAddr - SHIFT_SIZE)) {
		return errmsg("Cannot shift device data sections, fw image is too big.");
	}
	// for each device data section move it by an offset of 60kb (0xf000)

    PRINT_PROGRESS(progressFunc,(char*)"Shifting dev data section - ");

    // possible problem : if itoc array isnt ordered by ascending flash address and dev data sections are larger that 60kb
    // there is a chance we runover exsisting device data sections
    // Fix : preform the section shift by order from the lowest addresss to the highest.
    std::vector<struct toc_info*> sortedTocs(_fs3ImgInfo.numOfItocs);
    for (int i=0 ; i< _fs3ImgInfo.numOfItocs ; i++) {
    	sortedTocs[i]= &(_fs3ImgInfo.tocArr[i]);
    }
    std::sort(sortedTocs.begin(), sortedTocs.end(), TocComp(_fwImgInfo.imgStart));

    // shift the location of device data sections by SHIFT_SIZE (60kb)
    for (std::vector<struct toc_info*>::iterator it = sortedTocs.begin() ; it != sortedTocs.end(); it++) {
        if ((*it)->toc_entry.device_data) {
            // update the itoc (basically update the flash_addr and itoc entry crc)
            struct toc_info *currToc = *it;
            if (!Fs3UpdateItocInfo(currToc, ((currToc->toc_entry.flash_addr << 2) - SHIFT_SIZE))) {
                PRINT_PROGRESS(progressFunc,(char*)"FAILED\n");
                return false;
            }
            // write the section to its new place in the flash
            if (!writeImage((ProgressCallBack)NULL, getAbsAddr(currToc) , (u_int8_t*)&currToc->section_data[0], (currToc->toc_entry.size << 2), true)) {
                PRINT_PROGRESS(progressFunc,(char*)"FAILED\n");
                return false;
            }
        }
    }
    PRINT_PROGRESS(progressFunc,(char*)"OK\n");
    // update itoc section
    if (!reburnItocSection(progressFunc)) {
    	return false;
    }
    return true;
}


bool Fs3Operations::CheckItocArrConsistency(std::vector<struct toc_info*>& sortedTocVec, u_int32_t imageStartAddr) {
    u_int32_t sectEndAddr = 0, nextSectStrtAddr = 0;
    std::vector<struct toc_info*>::iterator it = sortedTocVec.begin(), itNext = sortedTocVec.begin();
    itNext++;
    for ( ; itNext != sortedTocVec.end(); it++, itNext++) {
        sectEndAddr = getAbsAddr(*it, imageStartAddr) + ((*it)->toc_entry.size << 2) - 1;
        nextSectStrtAddr = getAbsAddr(*itNext, imageStartAddr);
        if (sectEndAddr >= nextSectStrtAddr) {
            return errmsg("inconsistency found in ITOC. %s(0x%x) section will potentially overwrite %s(0x%x) section.",\
                    GetSectionNameByType((*it)->toc_entry.type), (*it)->toc_entry.type,\
                    GetSectionNameByType((*itNext)->toc_entry.type), (*itNext)->toc_entry.type);
        }
    }
    return true;
}


bool Fs3Operations::CheckItocArray()
{
    // sort the itocs
    std::vector<struct toc_info*> sortedTocs(_fs3ImgInfo.numOfItocs);
    for (int i=0 ; i< _fs3ImgInfo.numOfItocs ; i++) {
        sortedTocs[i]= &(_fs3ImgInfo.tocArr[i]);
    }
    std::sort(sortedTocs.begin(), sortedTocs.end(), TocComp(0));
    // check for inconsistency image burnt on 1st half
    if(!CheckItocArrConsistency(sortedTocs, 0)) {
        return false;
    }

    std::sort(sortedTocs.begin(), sortedTocs.end(), TocComp((1 << _fwImgInfo.cntxLog2ChunkSize)));
    // check for inconsistency image burn on second half
    if(!CheckItocArrConsistency(sortedTocs, (1 << _fwImgInfo.cntxLog2ChunkSize))) {
        return false;
    }
    return true;
}

const char* Fs3Operations::FwGetResetRecommandationStr()
{
#if defined(_WIN_) || defined(MST_UL)
    // mlxfwreset tool not supported for windows yet
    return (const char*)NULL;
#endif

    if (!_isfuSupported) {
        return (const char*)NULL;
    }
    return "To load new FW run mlxfwreset or reboot machine.";
}

bool Fs3Operations::Fs3IsfuActivateImage(u_int32_t newImageStart)
{
    int rc = 0;
    mfile *mf = (mfile*)NULL;
    struct cibfw_register_mfai mfai;
    struct cibfw_register_mfrl mfrl;
    memset(&mfai, 0, sizeof(mfai));
    memset(&mfrl, 0, sizeof(mfrl));

    if (!_devName) {// not an mst device
        return true;
    }
    // send MFRL register
    mf = mopen(_devName);
    if (!mf) {
        return false;
    }
    mfai.address = newImageStart;
    mfai.use_address = 1;
    rc = reg_access_mfai(mf,REG_ACCESS_METHOD_SET, &mfai);
    if (rc) {
        goto cleanup;
    }
    // send warm boot (bit 6)
    mfrl.reset_level = 1 << 6;
    rc = reg_access_mfrl(mf,REG_ACCESS_METHOD_SET, &mfrl);
    // ignore ME_REG_ACCESS_BAD_PARAM error for old FW
    rc = (rc == ME_REG_ACCESS_BAD_PARAM) ? ME_OK : rc;
cleanup:
    mclose(mf);
    if (rc) {
        return errmsg("Failed to activate image. %s", m_err2str((MError)rc));
    }
    return true;
}


#define IS_EMPTY_CX4_MFG_UIDS(fs3_uids_info) \
        (((fs3_uids_info).cx4_uids.base_guid.uid == 0x0ULL) &&  ((fs3_uids_info).cx4_uids.base_mac.uid == 0x0ULL))


#define GET_UID_LOW(uid) \
        ((uid) & 0xffffffffULL)

#define GET_UID_HIGH(uid) \
        ((uid) >> 32)

bool Fs3Operations::CheckAndFixCX4(bool justCheck)
{
    // assuming query/verify was ran before and this is being called on an MST device during burn
    FBase* origFlashObj = (FBase*)NULL;

    if (!_ioAccess->is_flash()) {
        return true;
    }

    if (((Flash*)_ioAccess)->get_dev_id() != CX4_HW_ID || _maxImgLog2Size != 0x16) {
        // nothing to do return
        return true;
    }

    // re-open flash with -ocr if needed
    if (_fwParams.ignoreCacheRep == 0) {
        origFlashObj = _ioAccess;
        _fwParams.ignoreCacheRep = 1;
        if (!FwOperations::FwAccessCreate(_fwParams, &_ioAccess)) {
            _ioAccess = origFlashObj;
            _fwParams.ignoreCacheRep = 0;
            return errmsg("Failed to open device for direct flash access");
        }
    }
    // check if Flash0 last sub-sector is write protected (indication if we need to continue)
    ext_flash_attr_t attr;
    memset(&attr, 0, sizeof(attr));

    if (!((Flash*)_ioAccess)->get_attr(attr)) {
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to perform Flash operation");
    }

    if (attr.type_str) {
        delete [] attr.type_str;
    }

    if (!(attr.protect_info_array[0].is_subsector && attr.protect_info_array[0].sectors_num == 1 && attr.protect_info_array[0].is_bottom == 0)) {
        // Flash0 protection is not as expected : Flash0.Top,1-SubSectors. nothing to do
        FLASH_RESTORE(origFlashObj);
        return true;
    }
    // mlxmodfw has corrupted dev_data sections and flash protection. attempt to fix

    if (justCheck) {
        // just perform checks and return
        FLASH_RESTORE(origFlashObj);
        return false;
    }

    char paramName[50] = {0};
    char paramVal[50] = {0};
    bool rc = FixCX4Uids();

    // set write protection to last sector
    strncpy(paramName, "Flash0.WriteProtected", 50);
    strncpy(paramVal, "Top,1-Sectors", 50);
    if (!((Flash*)_ioAccess)->set_attr(paramName, paramVal)) {
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to perform Flash operation");
    }
    // wait for flash op to finish
    msleep(500);
    FLASH_RESTORE(origFlashObj);

    if (!rc || !Fs3IntQuery()) {
        return false;
    }
    return true;
}

bool Fs3Operations::FixCX4Uids()
{
    // fix mfg guids_num and step_size
    fs3_uid_t baseUid;
    memset(&baseUid, 0, sizeof(baseUid));
    if(IS_EMPTY_CX4_MFG_UIDS(_fs3ImgInfo.ext_info.orig_fs3_uids_info)) {
        // fix macs/guids
        // adrianc: if MFGs are zero: set the same uids as in dev_info.
        baseUid.base_guid.l = GET_UID_LOW(_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids.base_guid.uid);
        baseUid.base_guid.h = GET_UID_HIGH(_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids.base_guid.uid);

        baseUid.base_mac.l = GET_UID_LOW(_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids.base_mac.uid);
        baseUid.base_mac.h = GET_UID_HIGH(_fs3ImgInfo.ext_info.fs3_uids_info.cx4_uids.base_mac.uid);

        baseUid.base_guid_specified = 1;
        baseUid.base_mac_specified = 1;
        baseUid.set_mac_from_guid = 0;

        // set manufacture guids
        if (!FwSetMFG(baseUid)) {
            return false;
        }
    }
    return true;
}

#define OPEN_OCR(origFlashObj) do {\
                        origFlashObj = _ioAccess;\
                        _fwParams.ignoreCacheRep = 1;\
                        if (!FwOperations::FwAccessCreate(_fwParams, &_ioAccess)) {\
                            _ioAccess = origFlashObj;\
                            _fwParams.ignoreCacheRep = 0;\
                            return errmsg("Failed to open device for direct flash access");\
                        }\
                    } while (0)

#define SET_WRITE_PROTECT(name, val) do { \
                        char paramName[50] = {0};\
                        char paramVal[50] = {0};\
                        strncpy(paramName, name, 50);\
                        strncpy(paramVal, val, 50);\
                        if (!((Flash*)_ioAccess)->set_attr(paramName, paramVal)) {\
                            FLASH_RESTORE(origFlashObj);\
                            return errmsg("Failed to perform Flash operation");\
                        }\
                        msleep(500);\
                    } while (0)

bool Fs3Operations::FixCX4WriteProtection(bool justCheck)
{
    if (!_ioAccess->is_flash() || ((Flash*)_ioAccess)->get_dev_id() != CX4_HW_ID) {
        return true;
    }

    struct toc_info* mfgToc = (struct toc_info*)NULL;
    struct toc_info* vpdToc = (struct toc_info*)NULL;
    u_int32_t mfgAddr = 0;
    u_int32_t vpdAddr = 0;
    u_int32_t flashSize = 0;
    u_int32_t shiftSize = 0x8000;
    FBase* origFlashObj = (FBase*)NULL;
    std::vector<u_int8_t> newVpdSection;
    int retries = 0;
    if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, FS3_MFG_INFO, mfgToc)) {
        return errmsg("failed to locate MFG_INFO address within the FW image");
    }
    ext_flash_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    if (!((Flash*)_ioAccess)->get_attr(attr)) {
        return errmsg("Failed to perform Flash operation");
    }
    flashSize = attr.size;
    mfgAddr = getAbsAddr(mfgToc);
    /* If the flash size is 16-MB and the MFG at the end 0xff0000
     * Move it with the VPD the bottom 32-bit (shift 0x8000) and
     * FlashProtected Top,8-SubSectors
     */
    if (flashSize != 0x1000000 || mfgAddr != 0xff0000) {
        return true;
    }

    if (justCheck) {
        // just perform checks and return
        FLASH_RESTORE(origFlashObj);
        return false;
    }

    // get VPD section
    _readSectList.push_back(FS3_VPD_R0);
    if (!Fs3IntQuery()) {
        _readSectList.pop_back();
        return false;
    }
    _readSectList.pop_back();

    // re-open flash with -ocr if needed
    if (_fwParams.ignoreCacheRep == 0) {
        OPEN_OCR(origFlashObj);
    }
    /*
     * Check if write protected and disable it
     */
    if (((Flash*)_ioAccess)->is_flash_write_protected()) {
        SET_WRITE_PROTECT("Flash0.WriteProtected", "Disabled");
    }
    while (((Flash*)_ioAccess)->is_flash_write_protected() && retries < 5) {
        msleep(500);
    }
    if (retries == 5) {
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to disable flash write protection");
    }
    /*
     * MOVE MFG & VPD Sections to +0x8000
     */
    const char* flashParamName = "Flash0.WriteProtected";
    const char* flashParamVal = "Top,8-SubSectors";

    /*
     * Moving VPD section
     */
    if (!Fs3GetItocInfo(_fs3ImgInfo.tocArr, _fs3ImgInfo.numOfItocs, FS3_VPD_R0, vpdToc)) {
        SET_WRITE_PROTECT(flashParamName, flashParamVal);
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to locate VPD_R0 address within the FW image");
    }
    /*
     * VPD_R0 address is after the MFG_INFO section
     */
    vpdAddr = mfgAddr + mfgToc->toc_entry.size * 4;

    if (!Fs3UpdateItocInfo(vpdToc, vpdAddr + shiftSize)) {
        SET_WRITE_PROTECT(flashParamName, flashParamVal);
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to shift VPD_R0 section");
    }
    if (!Fs3ReburnItocSection(vpdAddr + shiftSize, vpdToc->toc_entry.size * 4, vpdToc->section_data, "VPD")) {
        SET_WRITE_PROTECT(flashParamName, flashParamVal);
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to burn VPD_R0 section, the image maybe left in bad situation");;
    }
    if (!Fs3UpdateItocInfo(mfgToc, mfgAddr + shiftSize)) {
        SET_WRITE_PROTECT(flashParamName, flashParamVal);
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to shift MFG_INFO section");
    }
    if (!Fs3ReburnItocSection(mfgAddr + shiftSize, mfgToc->toc_entry.size * 4, mfgToc->section_data, "GUID")) {
        SET_WRITE_PROTECT(flashParamName, flashParamVal);
        FLASH_RESTORE(origFlashObj);
        return errmsg("Failed to burn MFG_INFO section, the image maybe left in bad situation");
    }
    /*
     * Enable Flash protected
     */
    SET_WRITE_PROTECT(flashParamName, flashParamVal);
    FLASH_RESTORE(origFlashObj);
    /*
     * Query for check
     */
    if (!Fs3IntQuery()) {
        return errmsg("Failed to query device after fixing write protected sections");
    }
    return true;
}

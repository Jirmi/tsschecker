//
//  ipswme.c
//  tsschecker
//
//  Created by tihmstar on 07.01.16.
//  Copyright © 2016 tihmstar. All rights reserved.
//

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include "tsschecker.h"
#include "jsmn.h"
#include "download.h"
#include <libpartialzip-1.0/libpartialzip.h>
#include "tss.h"

#define FIRMWARE_JSON_PATH "/tmp/firmware.json"
#define FIRMWARE_JSON_URL "https://api.ipsw.me/v2.1/firmwares.json/condensed"
#define FIRMWARE_OTA_JSON_PATH "/tmp/ota.json"
#define FIRMWARE_OTA_JSON_URL "https://api.ipsw.me/v2.1/ota.json/condensed"
#define BBGCID_JSON_PATH "/tmp/bbgcid.json"
#define BBGCID_JSON_URL "http://api.tihmstar.net/bbgcid?condensed=1"

#define MANIFEST_SAVE_PATH "/tmp/tsschecker"
#define noncelen 20

#pragma mark getJson functions

int print_tss_request = 0;
int print_tss_response = 0;
int nocache = 0;
int save_shshblobs = 0;

char *getBBCIDJson(){
    info("[TSSC] opening bbgcid.json\n");
    FILE *f = fopen(BBGCID_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(BBGCID_JSON_URL, BBGCID_JSON_PATH);
        f = fopen(BBGCID_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fclose(f);
    return fJson;
}

char *getFirmwareJson(){
    info("[TSSC] opening firmware.json\n");
    FILE *f = fopen(FIRMWARE_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(FIRMWARE_JSON_URL, FIRMWARE_JSON_PATH);
        f = fopen(FIRMWARE_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fclose(f);
    return fJson;
}

char *getOtaJson(){
    info("[TSSC] opening ota.json\n");
    FILE *f = fopen(FIRMWARE_OTA_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(FIRMWARE_OTA_JSON_URL, FIRMWARE_OTA_JSON_PATH);
        f = fopen(FIRMWARE_OTA_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fclose(f);
    return fJson;
}

#pragma mark json functions

void printJString(jsmntok_t *str, char * firmwarejson){
    for (int j=0; j<str->end-str->start; j++) {
        putchar(*(firmwarejson+str->start + j));
    }
    putchar('\n');
}

jsmntok_t *objectForKey(jsmntok_t *tokens, char *firmwareJson, char*key){
    
    jsmntok_t *dictElements = tokens->value;
    for (jsmntok_t *tmp = dictElements; ; tmp = tmp->next) {
        
        if (strncmp(key, firmwareJson + tmp->start, strlen(key)) == 0) return tmp;
        
        if (tmp->next == dictElements) break;
    }
    
    return NULL;
}

int parseTokens(char *json, jsmntok_t **tokens){
    jsmn_parser parser;
    jsmn_init(&parser);
    
    log("[JSON] counting elements\n");
    unsigned int tokensCnt = jsmn_parse(&parser, json, strlen(json), NULL, 0);
    
    *tokens = (jsmntok_t*)malloc(sizeof(jsmntok_t) * tokensCnt);
    jsmn_init(&parser);
    log("[JSON] parsing elements\n");
    return jsmn_parse(&parser, json, strlen(json), *tokens, tokensCnt);
}

#pragma mark get functions

char *getFirmwareUrl(char *device, char *version,char *firmwarejson, jsmntok_t *tokens, int isOta, int useBeta){
    
    jsmntok_t *devices = (isOta) ? tokens : objectForKey(tokens, firmwarejson, "devices");
    jsmntok_t *mydevice = objectForKey(devices, firmwarejson, device);
    jsmntok_t *firmwares = objectForKey(mydevice, firmwarejson, "firmwares");
    
    for (jsmntok_t *tmp = firmwares->value; tmp != NULL; tmp = tmp->next) {
        jsmntok_t *ios = objectForKey(tmp, firmwarejson, "version");
        if (ios->value->end - ios->value->start == strlen(version) && strncmp(version, firmwarejson + ios->value->start, strlen(version)) == 0) {
            
            if (isOta) {
                jsmntok_t *releaseType = NULL;
                if (useBeta && !(releaseType = objectForKey(tmp, firmwarejson, "releasetype"))) continue;
                else if (!useBeta);
                else if (strncmp(firmwarejson + releaseType->value->start, "Beta", releaseType->value->end - releaseType->value->start) != 0) continue;
            }
            
            jsmntok_t *url = objectForKey(tmp, firmwarejson, "url")->value;
            
            char *ret = malloc(url->end - url->start+1);
            char *cpy = ret;
            memset(ret, 0, url->end - url->start+1);
            for (int i=url->start ; i< url->end; i++) *(cpy++) = firmwarejson[i];
            return ret;
        }
        
    }
    return NULL;
}

static void printline(int percent){
    info("%03d [",percent);for (int i=0; i<100; i++) putchar((percent >0) ? ((--percent > 0) ? '=' : '>') : ' ');
    info("]");
}

static void partialzip_callback(partialzip_t* info, partialzip_file_t* file, size_t progress){
    info("\x1b[A\033[J"); //clear 2 lines
    printline((int)progress);
    info("\n");
}

int downloadPartialzip(char *url, char *file, char *dst){
    log("[LPZP] downloading %s from %s\n",file,url);
    return partialzip_download_file(url, file, dst, &partialzip_callback);
}

char *getBuildManifest(char *url, int isOta){
    struct stat st = {0};
    
    //gen name
    size_t len = strlen(url)-1;
    char *name = NULL;
    while (url[--len] != '/') name = url+len;
    
    len = strlen(url+len) + strlen(MANIFEST_SAVE_PATH) + 1 +1;
    if (isOta) len += strlen("ota");
    char *fileDir = malloc(len);
    memset(fileDir, 0, len);
    
    strncat(fileDir, MANIFEST_SAVE_PATH, strlen(MANIFEST_SAVE_PATH));
    strncat(fileDir, "/", 1);
    strncat(fileDir, name, strlen(MANIFEST_SAVE_PATH));
    if (isOta) strncat(fileDir, "ota", strlen("ota"));

    memset(&st, 0, sizeof(st));
    if (stat(MANIFEST_SAVE_PATH, &st) == -1) mkdir(MANIFEST_SAVE_PATH, 0700);
    
    //get file
    info("[TSSC] opening Buildmanifest for %s\n",name);
    FILE *f = fopen(fileDir, "rb");
    
    if (!f || nocache){
        //download if it isn't there
        if (downloadPartialzip(url, (isOta) ? "AssetData/boot/BuildManifest.plist" : "BuildManifest.plist", fileDir)){
            free(fileDir);
            return NULL;
        }
        f = fopen(fileDir, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    //load it
    char *buildmanifest = malloc(fsize + 1);
    fread(buildmanifest, fsize, 1, f);
    buildmanifest[fsize] = '\0';
    fclose(f);
    
    
    free(fileDir);
    return buildmanifest;
}

int64_t getBBGCIDForDevice(char *deviceModel){
    int64_t bbgcid = 0;
#define reterror(a ... ) {error(a); bbgcid = -1; goto error;}
    
    char *myjson = getBBCIDJson();
    
    jsmntok_t *tokens = NULL;
    int cnt = parseTokens(myjson, &tokens);
    if (cnt < 1) reterror("[TSSC] ERROR: parsing bbgcid.json failed!\n");
    
    
    jsmntok_t *device = objectForKey(tokens, myjson, deviceModel);
    if (!device) {
        reterror("[TSSC] ERROR: device \"%s\" is not in bbgcid.json, which means it's BasebandGoldCertID isn't documented yet.\nIf you own such a device please consider contacting @tihmstar (tihmstar@gmail.com) to get instructions how to contribute to this project.\n",deviceModel);
    }
    if (device->type == JSMN_PRIMITIVE) {
        warning("[TSSC] WARNING: A BasebandGoldCertID is not required for %s\n",deviceModel);
        bbgcid = 0;
    }else{
        device = device->value;
        char * buf = malloc(device->end - device->size +1);
        strncpy(buf, myjson+device->start,device->end - device->size);
        buf[device->end - device->size] = 0;
        bbgcid = atoll(buf);
        free(buf);
    }
    
error:
    if (myjson) free(myjson);
    if (tokens) free(tokens);
    return bbgcid;
#undef reterror
}

void debug_plist(plist_t plist);

void getRandNum(char *dst, size_t size, int base){
    for (int i=0; i<size; i++) {
        int j;
        if (base == 256) dst[i] = arc4random() % base;
        else dst[i] = ((j = arc4random() % base) < 10) ? '0' + j : 'a' + j-10;
    }
}

#pragma mark tss functions

int tss_populate_devicevals(plist_t tssreq, uint64_t ecid, char *nonce, size_t nonce_size, char *sep_nonce, size_t sep_nonce_size, int image4supported){
    
    plist_dict_set_item(tssreq, "ApECID", plist_new_uint(ecid)); //0000000000000000
    if (nonce) {
        plist_dict_set_item(tssreq, "ApNonce", plist_new_data(nonce, nonce_size));//aa aa aa aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa
    }
    
    if (sep_nonce) {//aa aa aa aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa
        plist_dict_set_item(tssreq, "ApSepNonce", plist_new_data(sep_nonce, sep_nonce_size));
    }
    
    plist_dict_set_item(tssreq, "ApProductionMode", plist_new_bool(1));
    
    if (image4supported) {
        plist_dict_set_item(tssreq, "ApSecurityMode", plist_new_bool(1));
        plist_dict_set_item(tssreq, "ApSupportsImg4", plist_new_bool(1));
    } else {
        plist_dict_set_item(tssreq, "ApSupportsImg4", plist_new_bool(0));
    }
    
    return 0;
}

int tss_populate_basebandvals(plist_t tssreq, plist_t tssparameters, int64_t BbGoldCertId){
    plist_t parameters = plist_new_dict();
    char bbnonce[noncelen+1];
    char bbsnum[5];
    int64_t BbChipID = 0;
    
    getRandNum(bbnonce, noncelen, 256);
    getRandNum(bbsnum, 4, 256);
    
    int n=0; for (int i=1; i<7; i++) BbChipID += (arc4random() % 10) * pow(10, ++n);
    
    
    plist_dict_set_item(parameters, "BbNonce", plist_new_data(bbnonce, noncelen));
    plist_dict_set_item(parameters, "BbChipID", plist_new_uint(BbChipID));
    plist_dict_set_item(parameters, "BbGoldCertId", plist_new_uint(BbGoldCertId));
    plist_dict_set_item(parameters, "BbSNUM", plist_new_data(bbsnum, 4));
    
    /* BasebandFirmware */
    plist_t BasebandFirmware = plist_access_path(tssparameters, 2, "Manifest", "BasebandFirmware");
    if (!BasebandFirmware || plist_get_node_type(BasebandFirmware) != PLIST_DICT) {
        error("ERROR: Unable to get BasebandFirmware node\n");
        return -1;
    }
    plist_t bbfwdict = plist_copy(BasebandFirmware);
    BasebandFirmware = NULL;
    if (plist_dict_get_item(bbfwdict, "Info")) {
        plist_dict_remove_item(bbfwdict, "Info");
    }
    plist_dict_set_item(tssreq, "BasebandFirmware", bbfwdict);
    
    tss_request_add_baseband_tags(tssreq, parameters, NULL);
    return 0;
}

int tss_populate_random(plist_t tssreq, int is64bit, uint64_t ecid){
    char nonce[noncelen+1];
    char sep_nonce[noncelen+1];
    
    int n=0;
    if (!ecid) for (int i=0; i<16; i++) ecid += (arc4random() % 10) * pow(10, n++);
    getRandNum(nonce, noncelen, 256);
    getRandNum(sep_nonce, noncelen, 256);
    
    nonce[noncelen] = sep_nonce[noncelen] = 0;
    
    debug("[TSSR] ecid=%llu\n",ecid);
    debug("[TSSR] nonce=%s\n",nonce);
    debug("[TSSR] sepnonce=%s\n",sep_nonce);
    
    return tss_populate_devicevals(tssreq, ecid, nonce, noncelen, sep_nonce, noncelen, is64bit);
}



int tssrequest(plist_t *tssrequest, char *buildManifest, char *device, uint64_t ecid, int checkBaseband){
#define reterror(a) {error(a); error = -1; goto error;}
    int error = 0;
    plist_t manifest = NULL;
    plist_t tssparameter = plist_new_dict();
    plist_t tssreq = tss_request_new(NULL);
    
    plist_from_xml(buildManifest, (unsigned)strlen(buildManifest), &manifest);
    
    plist_t buildidentities = plist_dict_get_item(manifest, "BuildIdentities");
    if (!buildidentities || plist_get_node_type(buildidentities) != PLIST_ARRAY){
        reterror("[TSSR] Error: could not get BuildIdentities\n");
    }
    plist_t id0 = plist_array_get_item(buildidentities, 0);
    if (!id0 || plist_get_node_type(id0) != PLIST_DICT){
        reterror("[TSSR] Error: could not get id0\n");
    }
    plist_t manifestdict = plist_dict_get_item(id0, "Manifest");
    if (!manifestdict || plist_get_node_type(manifestdict) != PLIST_DICT){
        reterror("[TSSR] Error: could not get manifest\n");
    }
    plist_t sep = plist_dict_get_item(manifestdict, "SEP");
    int is64Bit = !(!sep || plist_get_node_type(sep) != PLIST_DICT);
    
    tss_populate_random(tssparameter,is64Bit,ecid);
    tss_parameters_add_from_manifest(tssparameter, id0);
    
    if (tss_request_add_common_tags(tssreq, tssparameter, NULL) < 0) {
        reterror("[TSSR] ERROR: Unable to add common tags to TSS request\n");
    }

    if (tss_request_add_ap_tags(tssreq, tssparameter, NULL) < 0) {
        reterror("[TSSR] ERROR: Unable to add common tags to TSS request\n");
    }
    
    if (is64Bit) {
        if (tss_request_add_ap_img4_tags(tssreq, tssparameter) < 0) {
            reterror("[TSSR] ERROR: Unable to add img4 tags to TSS request\n");
        }
    } else {
        if (tss_request_add_ap_img3_tags(tssreq, tssparameter) < 0) {
            reterror("[TSSR] ERROR: Unable to add img3 tags to TSS request\n");
        }
    }
    
    if (checkBaseband) {
        int64_t BbGoldCertId = getBBGCIDForDevice(device);
        if (BbGoldCertId == -1) {
            warning("[TSSR] WARNING: there was an error getting BasebandGoldCertID, continuing without requesting Baseband ticket\n");
        }else if (BbGoldCertId) {
            tss_populate_basebandvals(tssreq,tssparameter,BbGoldCertId);
            tss_request_add_baseband_tags(tssreq, tssparameter, NULL);
        }else{
            log("[TSSR] LOG: device %s doesn't need a Baseband ticket, continuing without requesting a Baseband ticket\n",device);
        }
    }else{
        info("[TSSR] User specified not to request a Baseband ticket.\n");
    }
    
    
    *tssrequest = tssreq;
error:
    if (manifest) plist_free(manifest);
    if (tssparameter) plist_free(tssparameter);
    if (error) plist_free(tssreq), *tssrequest = NULL;
    return error;
#undef reterror
}

int isManifestBufSignedForDevice(char *buildManifestBuffer, char *device, uint64_t ecid, int checkBaseband){
    int isSigned = 0;
    plist_t tssreq = NULL;
    plist_t apticket = NULL;
    
    tssrequest(&tssreq, buildManifestBuffer, device, ecid, checkBaseband);
    isSigned = ((apticket = tss_request_send(tssreq, NULL)) > 0);
    
    
    if (print_tss_response) debug_plist(apticket);
    if (save_shshblobs){
        plist_t manifest = 0;
        plist_from_xml(buildManifestBuffer, (unsigned)strlen(buildManifestBuffer), &manifest);
        plist_t build = plist_dict_get_item(manifest, "ProductBuildVersion");
        char *cbuild = 0;
        plist_get_string_val(build, &cbuild);
        plist_t pvers = plist_dict_get_item(manifest, "ProductVersion");
        char *cpvers = 0;
        plist_get_string_val(pvers, &cpvers);
        
        plist_t pecid = plist_dict_get_item(tssreq, "ApECID");
        plist_get_uint_val(pecid, &ecid);
        char *cecid = ecid_to_string(ecid);
        
        
        uint32_t size = 0;
        char* data = NULL;
        plist_to_xml(apticket, &data, &size);
        
        size_t fnamelen = strlen(cecid) + strlen(device) + strlen(cpvers) + strlen(cbuild) + strlen("__-.shsh") + 1;
        char *fname = malloc(fnamelen);
        memset(fname, 0, fnamelen);
        snprintf(fname, fnamelen, "%s_%s_%s-%s.shsh",cecid,device,cpvers,cbuild);
        
        FILE *shshfile = fopen(fname, "w");
        fwrite(data, strlen(data), 1, shshfile);
        fclose(shshfile);
        
        info("Saved shsh blobs!\n");
        
        plist_free(manifest);
        free(fname);
        free(cpvers);
        free(cbuild);
        free(data);
    }
    
error:
    if (tssreq) plist_free(tssreq);
    if (apticket) plist_free(apticket);
    return isSigned;
}

int isManifestSignedForDevice(char *buildManifestPath, char **device, uint64_t ecid, int checkBaseband, char **version){
    int isSigned = 0;
#define reterror(a ...) {error(a); isSigned = -1; goto error;}
    plist_t manifest = NULL;
    plist_t ProductVersion = NULL;
    plist_t SupportedProductTypes = NULL;
    plist_t mDevice = NULL;
    
    info("[TSSC] opening %s\n",buildManifestPath);
    //filehandling
    FILE *fmanifest = fopen(buildManifestPath, "r");
    if (!fmanifest) reterror("[TSSC] ERROR: file %s nof found!\n",buildManifestPath);
    fseek(fmanifest, 0, SEEK_END);
    long fsize = ftell(fmanifest);
    fseek(fmanifest, 0, SEEK_SET);
    char *bufManifest = (char*)malloc(fsize + 1);
    fread(bufManifest, fsize, 1, fmanifest);
    fclose(fmanifest);
    
    plist_from_xml(bufManifest, (unsigned)strlen(bufManifest), &manifest);
    if (version && !*version){
        if (!manifest){
            warning("[TSSC] WARNING: could not find ProductVersion in BuildManifest.plist, failing to properly display iOS version\n");
        }else{
            ProductVersion = plist_dict_get_item(manifest, "ProductVersion");
            plist_get_string_val(ProductVersion, version);
        }
    }
    if (!*device) {
        if (manifest){
            SupportedProductTypes = plist_dict_get_item(manifest, "SupportedProductTypes");
            if (SupportedProductTypes) {
                mDevice = plist_array_get_item(SupportedProductTypes, 0);
                plist_get_string_val(mDevice, device);
            }
        }
        if (!*device) reterror("[TSSR] ERROR: device wasn't specified and could not be fetched from BuildManifest\n")
        else info("[TSSR] requesting ticket for %s\n",*device);
    }
    
    isSigned = isManifestBufSignedForDevice(bufManifest, *device, ecid, checkBaseband);
    
error:
    if (manifest) plist_free(manifest);
    free(bufManifest);
    return isSigned;
#undef reterror
}

int isVersionSignedForDevice(char *firmwareJson, jsmntok_t *firmwareTokens, char *version, char *device, uint64_t ecid, int otaFirmware, int checkBaseband, int useBeta){
    
    if (*version == '3' || *version - '0' < 3) {
        info("[TSSC] WARNING: version to check \"%s\" seems to be iOS 3 or lower, which did not require SHSH or APTicket.\n\tSkipping checks and returning true.\n",version);
        return 1;
    }
    
    int isSigned = 0;
#define reterror(a ... ) {error(a); goto error;}
    char *url = NULL;
    char *buildManifest = NULL;
    
    url = getFirmwareUrl(device, version, firmwareJson, firmwareTokens, otaFirmware, useBeta);
    if (!url) reterror("[TSSC] ERROR: could not get url for device %s on iOS %s\n",device,version);
    
    buildManifest = getBuildManifest(url, otaFirmware);
    if (!buildManifest) reterror("[TSSC] ERROR: could not get BuildManifest for firmwareurl %s\n",url);
    
    
    isSigned = isManifestBufSignedForDevice(buildManifest, device, ecid, checkBaseband);
    
error:
    if (url) free(url);
    if (buildManifest) free(buildManifest);
    return isSigned;
#undef reterror
}

#pragma mark print functions

#warning print devices function doesn't actually check if devices are sorted. it assues they are sorted in json
int printListOfDevices(char *firmwarejson, jsmntok_t *tokens){
#define MAX_PER_LINE 10
    log("[JSON] printing device list\n");
    int curr = 0;
    int currLen = 0;
    int rspn = 0;
    putchar('\n');
    jsmntok_t *ctok = objectForKey(tokens, firmwarejson, "devices");
    for (jsmntok_t *tmp = ctok->value; ; tmp = tmp->next) {
        if (!curr){
            curr = tmp->start;
            currLen = tmp->end - tmp->start;
        }else{
            for (int i = 0; i<currLen; i++) {
                char c = *(firmwarejson+curr+i);
                if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'))) break;
                if (c != *(firmwarejson+tmp->start+i)) {
                    putchar('\n');
                    putchar('\n');
                    curr = tmp->start;
                    currLen = tmp->end - tmp->start;
                    rspn = 0;
                    break;
                }
            }
        }
        for (int j=0; j<tmp->end-tmp->start; j++) {
            putchar(*(firmwarejson+tmp->start + j));
        }
        if (++rspn>= MAX_PER_LINE) putchar('\n'), rspn = 0; else putchar(' ');
        
        if (tmp->next == ctok->value) break;
    }
    putchar('\n');
    putchar('\n');
    return 0;
#undef MAX_PER_LINE
}

int cmpfunc(const void * a, const void * b){
    char *aa = *(char**)a;
    char *bb = *(char**)b;
    
    char *ac = strchr(aa, '[');
    char *bc = strchr(bb, '[');
    
    if ((ac != 0) ^ (bc != 0)) {
        *(ac + (long)bc) = 0;
        int ret = strcmp(*(char**)b, *(char**)a);
        *(ac + (long)bc) = '[';
        return ret ? ret : (int)ac - (int)bc;
    }
    
    return strcmp(*(char**)b, *(char**)a);
}

int printListOfiOSForDevice(char *firmwarejson, jsmntok_t *tokens, char *device, int isOTA){
#define MAX_PER_LINE 10
    jsmntok_t *firmwares = NULL;
    if (isOTA) {
        log("[JSON] printing ota list for device %s\n",device);
        jsmntok_t *mydevice = objectForKey(tokens, firmwarejson, device);
        firmwares = objectForKey(mydevice, firmwarejson, "firmwares");
        
    }else{
        log("[JSON] printing ipsw list for device %s\n",device);
        jsmntok_t *devices = objectForKey(tokens, firmwarejson, "devices");
        jsmntok_t *mydevice = objectForKey(devices, firmwarejson, device);
        firmwares = objectForKey(mydevice, firmwarejson, "firmwares");
    }
    
    int versionsCnt = firmwares->size;
    char **versions = (char**)malloc(versionsCnt * sizeof(char *));
    
    
    for (jsmntok_t *tmp = firmwares->value; tmp != NULL; tmp = tmp->next) {
        jsmntok_t *ios = objectForKey(tmp, firmwarejson, "version");
        
        int verslen= ios->value->end-ios->value->start;
        int isBeta = 0;
        jsmntok_t *releaseType = NULL;
        if ((releaseType = objectForKey(tmp, firmwarejson, "releasetype"))) {
            isBeta = (strncmp(firmwarejson + releaseType->value->start, "Beta", releaseType->value->end - releaseType->value->start) == 0);
        }
        
        versions[--versionsCnt] = (char*)malloc((verslen+1 + isBeta * strlen("[B]")) * sizeof(char));
        strncpy(versions[versionsCnt], firmwarejson + ios->value->start, verslen);
        if (isBeta) strncat(versions[versionsCnt], "[B]", strlen("[B]"));
        versions[versionsCnt][verslen+1 + isBeta * strlen("[B]")] = 0;
    }
    versionsCnt = firmwares->size;
    qsort(versions, versionsCnt, sizeof(char *), &cmpfunc);
    
    int rspn = 0;
    char currVer = 0;
    for (int i=0; i<versionsCnt; i++) {
        if (i){
            int res = strcmp(versions[i-1], versions[i]);
            free(versions[i-1]);
            if (res == 0) continue;
        }
        if (currVer && currVer != *versions[i]) printf("\n"), rspn = 0;
        currVer = *versions[i];
        if (!rspn) printf("[iOS %c] ",currVer);
        
        
        int printed = 0;
        printf("%s%n",versions[i],&printed);
        while (printed++ < 9) putchar(' ');
        if (++rspn>= MAX_PER_LINE) putchar('\n'), rspn = 0; else putchar(' ');
    }
    free(versions[versionsCnt-1]);
    free(versions);
    
    printf("\n\n");
    return 0;
#undef MAX_PER_LINE
}


#pragma mark check functions

int checkDeviceExists(char *device, char *firmwareJson, jsmntok_t *tokens, int isOta){
    jsmntok_t *ctok = (isOta) ? tokens : objectForKey(tokens, firmwareJson, "devices");
    for (jsmntok_t *tmp = ctok->value; ; tmp = tmp->next) {
        if (strncmp(device, firmwareJson+tmp->start, tmp->end - tmp->start) == 0) return 1;
        
        if (tmp->next == ctok->value) break;
    }
    
    return 0;
}

int checkFirmwareForDeviceExists(char *device, char *version, char *firmwareJson, jsmntok_t *tokens, int isOta){
    jsmntok_t *ctok = (isOta) ? tokens : objectForKey(tokens, firmwareJson, "devices");
    for (jsmntok_t *tmp = ctok->value; ; tmp = tmp->next) {
        if (strncmp(device, firmwareJson+tmp->start, tmp->end - tmp->start) == 0){
            jsmntok_t *firmwares = objectForKey(tmp, firmwareJson, "firmwares");
            
            for (jsmntok_t *tt = firmwares->value; tt ;tt = tt->next) {
                
                jsmntok_t *versiont = objectForKey(tt, firmwareJson, "version");
                size_t len = versiont->value->end - versiont->value->start;
                if (len == strlen(version) && strncmp(version, firmwareJson + versiont->value->start, len) == 0) return 1;
            }
            break;
        }
        if (tmp->next == ctok->value) break;
    }
    
    return 0;
}










/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <cJSON.h>

#include "ota_log.h"
#include "ota_transport.h"
#include "ota_service.h"
#include "ota_util.h"
#include "ota_version.h"
#include "ota_hal_os.h"
#include "ota_verify.h"
#include "iot_import.h"

#define POTA_FETCH_PERCENTAGE_MIN (0)
#define POTA_FETCH_PERCENTAGE_MAX (100)

#define MSG_REPORT_LEN  (256)
#define MSG_INFORM_LEN  (128)
#define MSG_LEN_MAX    (2048)

typedef enum {
    ALIOT_OTA_PROGRAMMING_FAILED = -4,
    ALIOT_OTA_CHECK_FAILED = -3,
    ALIOT_OTA_DOWNLOAD_FAILED = -2,
    ALIOT_OTA_UPGRADE_FAILED = -1,
} ALIOT_OTA_FAIL_E;

static const char *to_capital_letter(char *value, int len);
static int  ota_mqtt_gen_topic_name(char *buf, size_t buf_len, const char *ota_topic_type, const char *product_key,const char *device_name);
static void ota_mqtt_sub_callback(char *topic, int topic_len, void *payload, int payload_len, void *ccb);
static int  ota_mqtt_publish(const char *topic_type, const char *msg);
static int  ota_gen_info_msg(char *buf, size_t buf_len, uint32_t id, const char *version);
static int  ota_gen_report_msg(char *buf, size_t buf_len, uint32_t id, int progress, const char *msg_detail);
static bool ota_check_progress(int progress);

static const char *to_capital_letter(char *value, int len)
{
    if (value == NULL || len <= 0) {
        return NULL;
    }
    int i = 0;
    for (; i < len; i++) {
        if (*(value + i) >= 'a' && *(value + i) <= 'z') {
            *(value + i) -= 'a' - 'A';
        }
    }
    return value;
}

//Generate topic name according to @ota_topic_type, @product_key, @device_name
//and then copy to @buf.
//0, successful; -1, failed
static int ota_mqtt_gen_topic_name(char *buf, size_t buf_len, const char *ota_topic_type, const char *product_key,
                                   const char *device_name)
{
    int ret;
    ret = ota_snprintf(buf, buf_len, "/ota/device/%s/%s/%s", ota_topic_type, product_key, device_name);
    if (ret < 0) {
        OTA_LOG_E("ota_snprintf failed");
        return -1;
    }

    return 0;
}

static int ota_mqtt_publish(const char *topic_type, const char *msg)
{
    int ret = 0;
    char topic_name[OTA_MQTT_TOPIC_LEN] = {0};
    ota_service_manager* ctx = (ota_service_manager*)get_ota_service_manager();
    ret = ota_mqtt_gen_topic_name(topic_name, OTA_MQTT_TOPIC_LEN, topic_type, ctx->pk,ctx->dn);
    if (ret < 0) {
        OTA_LOG_E("generate topic name of info failed");
        return -1;
    }
    OTA_LOG_I("public topic=%s ,payload=%s\n", topic_name, msg);
    ret =  ota_hal_mqtt_publish(topic_name, 1, (void*)msg, strlen(msg) + 1);
    if (ret < 0) {
        OTA_LOG_E("publish failed");
        return -1;
    }

    return 0;
}

static void ota_mqtt_sub_callback(char *topic, int topic_len, void *payload, int payload_len, void *ccb)
{
    ota_cloud_cb_t ota_update = (ota_cloud_cb_t )ccb;
    if (!ota_update) {
        OTA_LOG_E("aliot_mqtt_ota_callback  pcontext null");
        return;
    }

    ota_update(UPGRADE_DEVICE , payload);
}

static int ota_gen_info_msg(char *buf, size_t buf_len, uint32_t id, const char *version)
{
    int ret;
    ret = ota_snprintf(buf,buf_len,
                   "{\"id\":%d,\"params\":{\"version\":\"%s\"}}",
                   id,version);

    if (ret < 0) {
        OTA_LOG_E("ota_snprintf failed");
        return -1;
    }

    return 0;
}

//Generate report information according to @id, @msg
//and then copy to @buf.
//0, successful; -1, failed
static int ota_gen_report_msg(char *buf, size_t buf_len, uint32_t id, int progress, const char *msg_detail)
{
    int ret;
    if (NULL == msg_detail) {
        ret = ota_snprintf(buf,buf_len,
                       "{\"id\":%d,\"params\":{\"step\": \"%d\",\"desc\":\"%d%%\"}}",
                       id,progress,progress);
    } else {
        ret = ota_snprintf(buf,buf_len,
                       "{\"id\":%d,\"params\":{\"step\": \"%d\",\"desc\":\"%s\"}}",
                       id,progress,msg_detail);
    }

    if (ret < 0) {
        OTA_LOG_E("ota_snprintf failed");
        return -1;
    } else if (ret >= buf_len) {
        OTA_LOG_E("msg is too long");
        return -1;
    }

    return 0;
}

//check whether the progress state is valid or not
//return: true, valid progress state; false, invalid progress state.
static bool ota_check_progress(int progress)
{
    OTA_LOG_I("ota_check_progress;%d", progress);
    return ((progress >= POTA_FETCH_PERCENTAGE_MIN)
            && (progress <= POTA_FETCH_PERCENTAGE_MAX));
}

static int8_t ota_parse_request(const char *request, int *buf_len, ota_request_params *request_parmas)
{
    return 0;
}

static int8_t ota_parse_response(const char *response, int buf_len, ota_response_params *response_parmas)
{
    OTA_LOG_I("parse response %s\n", response);
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        OTA_LOG_E("Error before: [%s]\n", cJSON_GetErrorPtr());
        goto parse_failed;
    } else {
        cJSON *message =  cJSON_GetObjectItem(root, "message");
        if (NULL == message) {
            OTA_LOG_E("invalid json doc of OTA ");
            goto parse_failed;
        }

        //check whether is positive message
        if ((strncasecmp(message->valuestring, "success", strlen("success")) )) {
            OTA_LOG_E("fail state of json doc of OTA");
            goto parse_failed;
        }

        cJSON *json_obj = cJSON_GetObjectItem(root, "data");
        if (!json_obj) {
            OTA_LOG_E("data back.");
            goto parse_failed;
        }

        cJSON *resourceUrl = cJSON_GetObjectItem(json_obj, "url");
        if (!resourceUrl) {
            OTA_LOG_E("resourceUrl back.");
            goto parse_failed;
        }

        cJSON *version = cJSON_GetObjectItem(json_obj, "version");
        if (!version) {
            OTA_LOG_E("version back.");
            goto parse_failed;
        }
        ota_set_version(version->valuestring);

#ifdef  OTA_MULTI_BINS
        char *upgrade_version = strtok(version->valuestring, "_");
        if (!upgrade_version) {
            strncpy(response_parmas->primary_version, version->valuestring,
                    (sizeof response_parmas->primary_version) - 1);
        } else {
            strncpy(response_parmas->primary_version, upgrade_version,
                    (sizeof response_parmas->primary_version) - 1);
            upgrade_version = strtok(NULL, "_");
            if (upgrade_version) {
                strncpy(response_parmas->secondary_version, upgrade_version,
                        (sizeof response_parmas->secondary_version) - 1);
            }
            OTA_LOG_I("response primary_version = %s, secondary_version = %s",
                      response_parmas->primary_version, response_parmas->secondary_version);
        }
#else
        strncpy(response_parmas->primary_version, version->valuestring,
                (sizeof response_parmas->primary_version) - 1);

#endif
        strncpy(response_parmas->download_url, resourceUrl->valuestring,
                (sizeof response_parmas->download_url) - 1);
        OTA_LOG_D(" response_parmas->download_url %s",response_parmas->download_url);

        cJSON *signMethod = cJSON_GetObjectItem(json_obj, "signMethod");
        if (signMethod) {//new protocol
            if (0 == strncasecmp(signMethod->valuestring, "Md5", strlen("Md5"))) {
                cJSON *md5 = cJSON_GetObjectItem(json_obj, "sign");
                if (!md5) {
                    OTA_LOG_E("no sign(md5) found");
                    goto parse_failed;
                }
                response_parmas->sign_method = MD5;
                strncpy(response_parmas->sign_value, md5->valuestring, OTA_MD5_LEN);
                response_parmas->sign_value[OTA_MD5_LEN] = '\0';
                to_capital_letter(response_parmas->sign_value, OTA_MD5_LEN);
            } else if (0 == strncasecmp(signMethod->valuestring, "Sha256", strlen("Sha256"))) {
                cJSON *sha256 = cJSON_GetObjectItem(json_obj, "sign");
                if (!sha256) {
                    OTA_LOG_E("no sign(sha256) found");
                    goto parse_failed;
                }

                response_parmas->sign_method = SHA256;
                strncpy(response_parmas->sign_value, sha256->valuestring, OTA_SHA256_LEN);
                response_parmas->sign_value[OTA_SHA256_LEN] = '\0';
                to_capital_letter(response_parmas->sign_value, OTA_SHA256_LEN);
            } else {
                OTA_LOG_E("get signMethod failed.");
                goto parse_failed;
            }

        } else {//old protocol
            cJSON *md5 = cJSON_GetObjectItem(json_obj, "md5");
            if (!md5) {
                OTA_LOG_E("no md5 found");
                goto parse_failed;
            }
            response_parmas->sign_method = MD5;
            strncpy(response_parmas->sign_value, md5->valuestring, OTA_MD5_LEN);
            response_parmas->sign_value[OTA_MD5_LEN] = '\0';
            to_capital_letter(response_parmas->sign_value, OTA_MD5_LEN);
        }

        cJSON *size = cJSON_GetObjectItem(json_obj, "size");
        if (!size) {
            OTA_LOG_E("size back.");
            goto parse_failed;
        }

        response_parmas->frimware_size = size->valueint;
        cJSON *diff = cJSON_GetObjectItem(json_obj, "isDiff");
        if (diff) {
            int is_diff = diff->valueint;
            ota_set_firmware_type(is_diff);
            if (is_diff) {
                cJSON *dmethod = cJSON_GetObjectItem(json_obj, "dmethod");
                if (dmethod) {
                    int diff_method = dmethod->valueint;
                    ota_set_diff_version(diff_method & 0xff);
                }
                cJSON *splictSize = cJSON_GetObjectItem(json_obj, "splictSize");
                if (splictSize) {
                    int splict_size = splictSize->valueint;
                    ota_set_splict_size(splict_size);
                }
            }
        }
    }
    
    OTA_LOG_D("parse_json success sign:%d value:%s\n",response_parmas->sign_method,response_parmas->sign_value);
    goto parse_success;

parse_failed:
    OTA_LOG_E("parse_json failed.");
    if (root) {
        cJSON_Delete(root);
    }
    return -1;

parse_success:
    if (root) {
        cJSON_Delete(root);
    }
    return 0;
}

static int8_t ota_parse_cancel_response(const char *response, int buf_len, ota_response_params *response_parmas)
{
    return 0;
}

static int8_t ota_publish_request(ota_request_params *request_parmas)
{
    return 0;
}

static int8_t ota_subscribe_upgrade(ota_cloud_cb_t msgCallback)
{
    int ret;
    char topic_name[OTA_MQTT_TOPIC_LEN] = {0};
    ota_service_manager* ctx = (ota_service_manager*)get_ota_service_manager();
    ret = ota_mqtt_gen_topic_name(topic_name, OTA_MQTT_TOPIC_LEN, "upgrade", ctx->pk,ctx->dn);
    if (ret < 0) {
        OTA_LOG_E("generate topic name of upgrade failed");
        return -1;
    }
    ret = ota_hal_mqtt_subscribe(topic_name, ota_mqtt_sub_callback, (void *)msgCallback);
    if (ret < 0) {
        OTA_LOG_E("mqtt subscribe failed:%s \n",topic_name);
        return -1;
    }

    return ret;
}

static int8_t ota_ustatus_post(int status, int progress)
{
    int ret = -1;
    char msg_reported[MSG_REPORT_LEN] = {0};
    if (!ota_check_progress(progress)) {
        OTA_LOG_E("progress is a invalid parameter");
        return ret;
    }

    if (status == OTA_CHECK_FAILED) {
        progress = ALIOT_OTA_CHECK_FAILED;
    } else if (status == OTA_DOWNLOAD_FAILED) {
        progress = ALIOT_OTA_DOWNLOAD_FAILED;
    } else if (status == OTA_DECOMPRESS_FAILED) {
        progress = ALIOT_OTA_PROGRAMMING_FAILED;
    } else if (status < 0) {
        progress = ALIOT_OTA_UPGRADE_FAILED;
    } else if (status == OTA_INIT) {
        progress = 0;
    }
    ret = ota_gen_report_msg(msg_reported, MSG_REPORT_LEN, 0, progress, NULL);

    if (0 != ret) {
        OTA_LOG_E("generate reported message failed");
        return -1;
    }

    ret = ota_mqtt_publish("progress", msg_reported);
    if (0 != ret) {
        OTA_LOG_E("Report progress failed");
        return -1;
    }
    return ret;
}


static int8_t ota_uresult_post(void)
{
    int ret = -1;
    char msg_informed[MSG_INFORM_LEN] = {0};
    ret = ota_gen_info_msg(msg_informed, MSG_INFORM_LEN, 0,
                           ota_get_system_version());
    if (ret != 0) {
        OTA_LOG_E("generate inform message failed");
        return -1;
    }
    ret = ota_mqtt_publish("inform", msg_informed);
    if (0 != ret) {
        OTA_LOG_E("Report version failed");
        return -1;
    }

    return ret;
}

static int8_t ota_cancel_upgrade(ota_cloud_cb_t msgCallback)
{
    return 0;
}

static const char *ota_get_uuid(void)
{
    ota_service_manager* ctx = (ota_service_manager*)get_ota_service_manager();
    return (const char *)ctx->uuid;
}

static int ota_transport_deinit(void)
{
    return ota_hal_mqtt_deinit_instance();
}

static int ota_transport_init(void)
{
    int ret = 0;
    ota_service_manager* ctx = (ota_service_manager*)get_ota_service_manager();
    OTA_LOG_E("mqtt init pk:%s dn:%s ds:%s\n", ctx->pk, ctx->dn, ctx->ds);
    ret = ota_hal_mqtt_init_instance(ctx->pk, ctx->dn, ctx->ds, MSG_LEN_MAX);
    if (ret < 0) {
        OTA_LOG_E("mqtt_init_instance failed\n");
        return -1;
    }
    return 0;
}

static ota_transport trans_mqtt = {
    .init = ota_transport_init,
    .parse_request = ota_parse_request,
    .parse_response = ota_parse_response,
    .parse_cancel_response = ota_parse_cancel_response,
    .subscribe_upgrade = ota_subscribe_upgrade,
    .cancel_upgrade = ota_cancel_upgrade,
    .publish_request = ota_publish_request,
    .status_post = ota_ustatus_post,
    .result_post = ota_uresult_post,
    .get_uuid = ota_get_uuid,
    .deinit = ota_transport_deinit,
};

const void * ota_get_transport_mqtt(void) {
    return &trans_mqtt;
}

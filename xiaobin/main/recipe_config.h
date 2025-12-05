// recipe_config.h - configuration for recipe recommendation service
#ifndef _RECIPE_CONFIG_H_
#define _RECIPE_CONFIG_H_

// Enable cloud recipe suggestions (set 0 to use local fallback only)
#define CLOUD_RECIPE_ENABLED 1

// Cloud recipe API URL (讯飞星火 Spark HTTP 接口)
#define RECIPE_API_URL "https://spark-api-open.xf-yun.com/v2/chat/completions"

// Credentials provided by user (Spark / 讯飞星火)
#define RECIPE_APPID "ba898f75"
#define RECIPE_API_KEY "0d68a07241dc5eb55a972b5f41a721d1"
#define RECIPE_API_SECRET "NGU0MDRmNmRjZWVmNTNmOTliZGMyYTE5"

#endif // _RECIPE_CONFIG_H_

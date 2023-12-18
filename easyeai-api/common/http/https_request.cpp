 /**
 *
 * Copyright 2021 by Guangzhou Easy EAI Technologny Co.,Ltd.
 * website: www.easy-eai.com
 *
 * Author: Jiehao.Zhong <zhongjiehao@easy-eai.com>
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * License file for more details.
 * 
 */

//===========================================system===========================================
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>

#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <termios.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>
#include <iconv.h>
#include <fcntl.h>
#include <dirent.h>
#include <dirent.h>
#include <semaphore.h>

//=========================================== C++ ===========================================
#include <string>
#include <queue>

//========================================== https ===========================================
#include "curl/curl.h"
#include "httplib.h"
#include "https_request.h"

using namespace std;
using namespace httplib;

static std::string g_ca_cert_file = CA_CERT_FILE;

static std::string sendDataToHttps(const char *server, const char *func, std::queue<string> keyList, std::queue<string> valueList)
{
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if(0 != access(g_ca_cert_file.c_str(), F_OK)){
        printf("[Certificate File Not Exists] path :(%s)\n", g_ca_cert_file.c_str());
    }
    
    httplib::SSLClient cli(server);
    cli.set_ca_cert_path(g_ca_cert_file.c_str());
    cli.enable_server_certificate_verification(true);
	
    MultipartFormDataItems items1;
    MultipartFormData pairData;
    std::string key;
    std::string value;
    while(1){
        pairData.name.clear();
        pairData.content.clear();
        pairData.filename.clear();
        pairData.content_type.clear();

        if(keyList.empty() || valueList.empty())
            break;

        key = keyList.front();
        keyList.pop();
        pairData.name.append(key);

        value = valueList.front();
        valueList.pop();
        pairData.content.append(value);

        items1.push_back(pairData);
    }

    auto boundary = std::string("+++++");
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    std::string strfunc;
    std::string body;
    std::string contents;

    std::string resBody;
    resBody.clear();

    for (const auto &item : items1) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + item.name + "\"";
        if (!item.filename.empty()) {
            body += "; filename=\"" + item.filename + "\"";
        }
        body += "\r\n";
        if (!item.content_type.empty()) {
            body += "Content-Type: " + item.content_type + "\r\n";
        }
        body += "\r\n";
        if (item.content.empty()) {
          //cout << "item.content.empty()" << endl;

          ifstream  in("1.png", std::ios::in | std::ios::binary); //打开输入文件
          in.seekg(0, std::ios::end);
          contents.resize(in.tellg());
          in.seekg(0, std::ios::beg);
          in.read(&contents[0], contents.size());
          in.close();
          body += contents + "\r\n";
        }
        else {
          //cout << "item.content not empty()" << endl;
          body += item.content + "\r\n";
        }
    }
    body += "--" + boundary + "--\r\n";


    strfunc.clear();
    strfunc.append(func);
//    cout << strfunc << endl;
    cout << body << endl;
    if (auto res = cli.Post(strfunc.c_str(), body, content_type.c_str())) {
        cout << res->status << endl;
        cout << res->get_header_value("Content-Type") << endl;
//        cout << res->body << endl;

        if(200 == res->status)
            resBody = res->body;
    } else {
        cout << "error code: " << res.error() << std::endl;
    }
#else
    std::string resBody;
    resBody.clear();
    printf("not support https version\n");
#endif
    return resBody;
}

static std::string sendDataToHttp(const char *server, const char *func, std::queue<string> keyList, std::queue<string> valueList)
{
    httplib::Client cli(server);
	
    MultipartFormDataItems items1;
    MultipartFormData pairData;
    std::string key;
    std::string value;
    while(1){
        pairData.name.clear();
        pairData.content.clear();
        pairData.filename.clear();
        pairData.content_type.clear();

        if(keyList.empty() || valueList.empty())
            break;

        key = keyList.front();
        keyList.pop();
        pairData.name.append(key);

        value = valueList.front();
        valueList.pop();
        pairData.content.append(value);

        items1.push_back(pairData);
    }

    auto boundary = std::string("+++++");
    std::string content_type = "multipart/form-data; boundary=" + boundary;

    std::string strfunc;
    std::string body;
    std::string contents;

    std::string resBody;
    resBody.clear();

    for (const auto &item : items1) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + item.name + "\"";
        if (!item.filename.empty()) {
            body += "; filename=\"" + item.filename + "\"";
        }
        body += "\r\n";
        if (!item.content_type.empty()) {
            body += "Content-Type: " + item.content_type + "\r\n";
        }
        body += "\r\n";
        if (item.content.empty()) {
          //cout << "item.content.empty()" << endl;

          ifstream  in("1.png", std::ios::in | std::ios::binary); //打开输入文件
          in.seekg(0, std::ios::end);
          contents.resize(in.tellg());
          in.seekg(0, std::ios::beg);
          in.read(&contents[0], contents.size());
          in.close();
          body += contents + "\r\n";
        }
        else {
          //cout << "item.content not empty()" << endl;
          body += item.content + "\r\n";
        }
    }
    body += "--" + boundary + "--\r\n";


    strfunc.clear();
    strfunc.append(func);
//    cout << strfunc << endl;
//    cout << body << endl;
    if (auto res = cli.Post(strfunc.c_str(), body, content_type.c_str())) {
        cout << res->status << endl;
        cout << res->get_header_value("Content-Type") << endl;
//        cout << res->body << endl;

        if(200 == res->status)
            resBody = res->body;
    } else {
        cout << "error code: " << res.error() << std::endl;
    }
    return resBody;
}

void set_customer_crt(const char *crt_file)
{
	if(NULL == crt_file)
		return ;
	
	g_ca_cert_file.clear();
	g_ca_cert_file.append(crt_file);	
}

static std::queue<std::string> g_keyList;
static std::queue<std::string> g_valueList;
void clear_multipart()
{
	std::queue<std::string> keyEmpty;
	std::queue<std::string> valEmpty;
	swap(keyEmpty, g_keyList);
	swap(valEmpty, g_valueList);
	
	return ;
}

void add_multipart_form_data(const char *key, const char *value)
{
	std::string strKey;
	strKey.clear();
	strKey.append(key);
	
	std::string strValue;
	strValue.clear();
	strValue.append(value);
	
	g_keyList.push(strKey);
	g_valueList.push(strValue);
	
	return ;
}

int32_t send_data_to_Https(const char *server, const char *func, char *result, uint32_t result_length)
{
	int minResultLength = result_length;
	
	std::string strResult = sendDataToHttps(server, func, g_keyList, g_valueList);
	if(strResult.empty())
		return -1;
	
	if(strResult.length() <= result_length){
		minResultLength = strResult.length();
	}
	
	if(NULL == result)
		return -1;
	
	memcpy(result, strResult.c_str(), minResultLength);
	
	clear_multipart();
	
	return 0;
}

int32_t send_data_to_Http(const char *server, const char *func, char *result, uint32_t result_length)
{
	int minResultLength = result_length;
	
	std::string strResult = sendDataToHttp(server, func, g_keyList, g_valueList);
	if(strResult.empty())
		return -1;
	
	if(strResult.length() <= result_length){
		minResultLength = strResult.length();
	}
	
	if(NULL == result)
		return -1;
	
	memcpy(result, strResult.c_str(), minResultLength);
	
	clear_multipart();
	
	return 0;
}

static char *g_pResult = NULL;
static size_t g_resLength = 0;
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    //printf("======Http_callback_start======\n");
    size_t data_len = size * nmemb;
    if(g_resLength <= data_len){
        if(g_pResult){
            free(g_pResult); g_pResult = NULL;
        }
    }

    if(NULL == g_pResult){
        g_resLength = data_len;
        g_pResult = (char *)malloc(g_resLength);
    }

    memset(g_pResult, 0, g_resLength);
    strcpy(g_pResult, ptr);

    
    //printf("%s\n",ptr);
    //printf("======Http_callback_end======\n");
    return data_len;
}

int32_t send_json_to_Https(const char *server, const char *func, const char *json, char *result, uint32_t result_length)
{
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if(0 != access(g_ca_cert_file.c_str(), F_OK)){
        printf("[Certificate File Not Exists] path :(%s)\n", g_ca_cert_file.c_str());
        return -1;
    }

    httplib::SSLClient cli(server);
    cli.set_ca_cert_path(g_ca_cert_file.c_str());
    cli.enable_server_certificate_verification(true);
	
	int minResultLength = result_length;
    
    std::string content_type = "application/json";

    std::string strfunc;
    std::string body;

    std::string resBody;
    resBody.clear();

    strfunc.clear();
    strfunc.append(func);
//    cout << strfunc << endl;

    body.clear();
    body.append(json);
//    cout << body << endl;

    /*
     * Send data
     */ 
    if (auto res = cli.Post(strfunc.c_str(), body, content_type.c_str())) {
        cout << res->status << endl;
        cout << res->get_header_value("Content-Type") << endl;
//        cout << res->body << endl;

        if(200 == res->status)
            resBody = res->body;
    } else {
        cout << "error code: " << res.error() << std::endl;
    }

    /*
     * handle Response
     */
	if(resBody.empty())
		return 0;

	if(resBody.length() <= result_length){
		minResultLength = resBody.length();
	}

	if(NULL == result)
		return -1;

	memcpy(result, resBody.c_str(), minResultLength);
    
	return 0;
#else
    printf("not support https version\n");
	return -1;
#endif
}

int32_t send_json_to_Http(const char *server, const char *func, const char *json, char *result, uint32_t result_length)
{
#if 0
    httplib::Client cli(server);
	
	int minResultLength = result_length;
    
    std::string content_type = "application/json";

    std::string strfunc;
    std::string body;

    std::string resBody;
    resBody.clear();

    strfunc.clear();
    strfunc.append(func);
//    cout << strfunc << endl;

    body.clear();
    body.append(json);
//    cout << body << endl;

    /*
     * Send data
     */ 
    if (auto res = cli.Post(strfunc.c_str(), body, content_type.c_str())) {
        cout << res->status << endl;
        cout << res->get_header_value("Content-Type") << endl;
//        cout << res->body << endl;

        if(200 == res->status)
            resBody = res->body;
    } else {
        cout << "error code: " << res.error() << std::endl;
    }

    /*
     * handle Response
     */
	if(resBody.empty())
		return 0;

	if(resBody.length() <= result_length){
		minResultLength = resBody.length();
	}

	if(NULL == result)
		return -1;

	memcpy(result, resBody.c_str(), minResultLength);
    
	return 0;
#else
    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Curl initialization failed");
        return -1;
    }

    char url[2048]={0};
    strcpy(url, server);
    strcpy(url+strlen(server), func);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1);
    
    //打印调试信息
    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        printf("curl_easy_perform() failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
	int minResultLength = result_length;
	if(g_resLength <= result_length){
		minResultLength = g_resLength;
	}
	memcpy(result, g_pResult, minResultLength);
    
    return 0;
#endif
}

int32_t get_file_from_https(const char *url, const char *func, const char *filePath)
{
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient cli(url);
    cli.set_ca_cert_path(g_ca_cert_file.c_str());
    cli.enable_server_certificate_verification(true);

    printf("url: %s\n", url);
    printf("func: %s\n", func);
    std::string strfunc = func;

    if (auto res = cli.Get(strfunc.c_str())) {
        cout << "https res : " << res->status << endl;
        if(200 == res->status){
//            int contentLength = atoi(res->get_header_value("Content-Length").c_str());
//            printf("-- body length : %d\n", contentLength);
            ofstream ofs(filePath, ios::out | ios::binary); //二进制保存文件
//            ofs.write(res->body.c_str(), contentLength);
            ofs << res->body;
            ofs.close();
        }

    } else {
        cout << "https error code: " << res.error() << std::endl;
        return (int)res.error();
    }
    return 0;
#else
	return -1;
#endif
}

int32_t get_file_from_http(const char *url, const char *func, const char *filePath)
{
    httplib::Client cli(url);

    printf("url: %s\n", url);
    printf("func: %s\n", func);
    std::string strfunc = func;

    if (auto res = cli.Get(strfunc.c_str())) {
        cout << "http res : " << res->status << endl;
        if(200 == res->status){
//            int contentLength = atoi(res->get_header_value("Content-Length").c_str());
//            printf("-- body length : %d\n", contentLength);
            ofstream ofs(filePath, ios::out | ios::binary); //二进制保存文件
//            ofs.write(res->body.c_str(), contentLength);
            ofs << res->body;
            ofs.close();
        }
    } else {
        cout << "http error code: " << res.error() << std::endl;
        return (int)res.error();
    }
    return 0;
}






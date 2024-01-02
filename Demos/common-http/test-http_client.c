#include <stdio.h>

#include "https_request.h"

#define FILE_PATH    "/userdata/Demo/upgrade.bin"

int main(void)
{
	set_customer_crt("/userdata/customer.crt");
	
	char res[4096] = {0};
	printf("===================================================================== : %s\n",res);
    
#if 0
// =========== form data =========== 
	clear_multipart();
	add_multipart_form_data("name", "gzlmo");
	add_multipart_form_data("id", "888888");
	add_multipart_form_data("pwd", "123456");
	
	send_data_to_Http("127.0.0.1:50000", "/add", res, sizeof(res));
	//send_data_to_Https("127.0.0.1:50000", "/add", res, sizeof(res));
	
#else
// ============= JSON ============== 
	send_json_to_Http("127.0.0.1:50000", "/test", "{ \"name\" : \"gzlmo\" }", res, sizeof(res));
	//send_json_to_Https("127.0.0.1:50000", "/test", "{ \"name\" : \"gzlmo\" }", res, sizeof(res));
#endif

    printf("result : \n%s\n",res);
	//get_file_from_http("127.0.0.1:50000", "/pic", FILE_PATH);
	//get_file_from_https("127.0.0.1:50000", "/pic", FILE_PATH);
	
	return 0;
}

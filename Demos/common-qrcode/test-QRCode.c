#include <stdio.h>

#include "qrcode.h"

#define QRCODE_PATH  "./Release/QRCode.png"

int main(void)
{
    StrToQRCode(QRCODE_PATH, "hello! my name is EasyEai-api !");
	
	return 0;
}

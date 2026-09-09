#include <fstream>
#include "httplib.h"
#include <iostream>
using namespace httplib;
using namespace std;

#define DATA_CHUNK_SIZE 4

int main(void) 
{
    Server svr;

    svr.Post("/test",  [](const Request &req, Response &res) {
        cout << "------ Test ------" << endl;
        auto strJSON = req.body;
        cout  << "========= strJSON: \n" << strJSON << endl;

        res.set_content("done", "text/plain");
    });

  svr.listen("0.0.0.0", 50000);
}


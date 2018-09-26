#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "../vendor/rapidjson/include/rapidjson/document.h"
#include "../vendor/rapidjson/include/rapidjson/filewritestream.h"
#include "../vendor/rapidjson/include/rapidjson/writer.h"

#define DEFAULT_SIZE   5000
#define DEFAULT_SLICES 5
#define WRITE_BUF_SIZE 65536

struct curl_response
{
    curl_response()
    {
        data = reinterpret_cast<char*>(std::malloc(1));
        data_size = 0;
    }

    ~curl_response()
    {
        std::free(data);
    }

    char* data;
    size_t data_size;
};

class curl_wrap
{
public:
    curl_wrap()
    {
        crl = curl_easy_init();
    }

    ~curl_wrap()
    {
        curl_easy_cleanup(crl);
    }

    void post_data(
        std::string   const& url,
        std::string   const& body,
        curl_response & resp)
    {
        curl_easy_setopt(crl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(crl, CURLOPT_WRITEFUNCTION, &write_data);
        curl_easy_setopt(crl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&resp));
        curl_easy_setopt(crl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_perform(crl);
    }

private:
    static size_t write_data(
        void   * buffer,
        size_t   size,
        size_t   nmemb,
        void   * userp)
    {
        curl_response* resp = reinterpret_cast<curl_response*>(userp);

        const char* real_buffer = reinterpret_cast<const char*>(buffer);
        size_t real_size = size * nmemb;

        // Realloc new data
        void* ptr = std::realloc(resp->data, resp->data_size + real_size + 1);

        resp->data = reinterpret_cast<char*>(ptr);

        std::memcpy(&(resp->data[resp->data_size]), buffer, real_size);

        resp->data_size += real_size;
        resp->data[resp->data_size] = 0;

        return real_size;
    }

    CURL* crl;
};

struct dump_options
{
    std::string host;
    std::string index;
    std::string filename;
    int slice_id;
    int slice_max;
    int size;
};

struct file_output
{
    file_output(std::string const& filename)
        : fd(fopen(filename.c_str(), "w")),
        stream(fd, buffer, sizeof(buffer))
    {
    }

    ~file_output()
    {
        fclose(fd);
    }

private:
    FILE* fd;
    char buffer[WRITE_BUF_SIZE];

public:
    rapidjson::FileWriteStream stream;
};

void write_document(
    rapidjson::FileWriteStream &      stream,
    rapidjson::Document        const& document,
    int                        *      hits_count,
    std::string                *      scroll_id)
{
    // Epic const unfolding.
    auto const& scroll_id_value   = document["_scroll_id"];
    auto const& hits_object_value = document["hits"];
    auto const& hits_object       = hits_object_value.GetObject();
    auto const& hits_value        = hits_object["hits"];
    auto const& hits              = hits_value.GetArray();

    for (rapidjson::Value const& hit : hits)
    {
        rapidjson::Writer<rapidjson::FileWriteStream> writer(stream);
        hit.Accept(writer);

        // Put a single new-line at the end to create a new-line
        // separated JSON file.
        stream.Put('\n');
    }

    *scroll_id  = scroll_id_value.GetString();
    *hits_count = hits.Size();
}

void output_parser_error(
    rapidjson::Document const& doc)
{
    std::cerr << "JSON parsing failed with code '"
              << doc.GetParseError()
              << "' at offset "
              << doc.GetErrorOffset() << std::endl;
}

void dump(
    dump_options const& options)
{
    curl_wrap crl;
    file_output output(options.filename);

    std::string query = "{\n"
        "\"size\": " + std::to_string(options.size) + ",\n"
        "\"slice\": {\n"
            "\"id\": " + std::to_string(options.slice_id) + ",\n"
            "\"max\": " + std::to_string(options.slice_max) + "\n"
        "}\n"
    "}";

    curl_response resp;
    crl.post_data(options.host + "/" + options.index + "/_search?scroll=1m", query, resp);

    rapidjson::Document doc;
    doc.Parse(resp.data ,resp.data_size);

    if (doc.HasParseError())
    {
        return output_parser_error(doc);
    }

    std::string scroll_id;
    int         hits_count;

    write_document(
        output.stream,
        doc,
        &hits_count,
        &scroll_id);

    do
    {
        query = "{\n"
            "\"scroll\": \"1m\",\n"
            "\"scroll_id\": \"" + scroll_id + "\"\n"
        "}\n";

        curl_response resp2;
        crl.post_data(options.host + "/_search/scroll", query, resp2);

        rapidjson::Document sdoc;
        sdoc.Parse(resp2.data, resp2.data_size);

        if (sdoc.HasParseError())
        {
            return output_parser_error(sdoc);
        }

        write_document(
            output.stream,
            sdoc,
            &hits_count,
            &scroll_id);
    } while (hits_count > 0);
}

bool find_cmd_option(
    std::vector<std::string> const& args,
    std::string              const& name,
    std::string              &      out)
{
    for (std::string const& arg : args)
    {
        if (arg.substr(0, name.size()) != name)
        {
            continue;
        }

        out = arg.substr(name.size() + 1);

        return true;
    }

    return false;
}

int main(
    int    argc,
    char * argv[])
{
    curl_global_init(CURL_GLOBAL_ALL);

    std::vector<std::string> args(argv + 1, argv + argc);
    std::vector<std::thread> threads;

    // Parse command line options
    std::string host;
    if (!find_cmd_option(args, "--host", host))
    {
        std::cerr << "Argument --host <host> required." << std::endl;
        return 1;
    }

    std::string index;
    if (!find_cmd_option(args, "--index", index))
    {
        std::cerr << "Argument --index <index> required." << std::endl;
        return 1;
    }

    std::string slices;
    if (!find_cmd_option(args, "--slices", slices))
    {
        slices = std::to_string(DEFAULT_SLICES);
    }

    std::string size;
    if (!find_cmd_option(args, "--size", size))
    {
        size = std::to_string(DEFAULT_SIZE);
    }

    int num_size = std::atoi(size.c_str());
    int num_slices = std::atoi(slices.c_str());

    for (int i = 0; i < num_slices; i++)
    {
        std::stringstream filename;
        filename << index << "." << i << ".json";

        dump_options opts;
        opts.filename  = filename.str();
        opts.host      = host;
        opts.index     = index;
        opts.size      = num_size;
        opts.slice_id  = i;
        opts.slice_max = num_slices;

        threads.push_back(std::thread(dump, opts));
    }

    for (auto& th : threads)
    {
        th.join();
    }

    curl_global_cleanup();

    return 0;
}

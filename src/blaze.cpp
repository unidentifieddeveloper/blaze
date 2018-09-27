#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
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

std::mutex mtx_out;

struct thread_state
{
    std::stringstream error;
};

struct thread_container
{
    int slice_id;
    thread_state state;
    std::thread thread;
};

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
    long response_code;
    std::string error;
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

    bool post_data(
        std::string   const& url,
        std::string   const& body,
        curl_response & resp)
    {
        curl_easy_setopt(crl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(crl, CURLOPT_WRITEFUNCTION, &write_data);
        curl_easy_setopt(crl, CURLOPT_WRITEDATA,     reinterpret_cast<void*>(&resp));
        curl_easy_setopt(crl, CURLOPT_POSTFIELDS,    body.c_str());

        CURLcode res = curl_easy_perform(crl);

        if (res == CURLE_OK)
        {
            curl_easy_getinfo(crl, CURLINFO_RESPONSE_CODE, &resp.response_code);
            return true;
        }

        resp.error = curl_easy_strerror(res);
        return false;
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
    int         slice_id;
    int         slice_max;
    int         size;
};

void write_document(
    rapidjson::Document        & document,
    int                        * hits_count,
    std::string                * scroll_id)
{
    std::unique_lock<std::mutex> lock(mtx_out);

    static char                       buffer[WRITE_BUF_SIZE];
    static rapidjson::FileWriteStream stream(stdout, buffer, sizeof(buffer));

    // Epic const unfolding.
    auto const& scroll_id_value   = document["_scroll_id"];
    auto const& hits_object_value = document["hits"];
    auto const& hits_object       = hits_object_value.GetObject();
    auto const& hits_value        = hits_object["hits"];
    auto const& hits              = hits_value.GetArray();

    // Shared allocator
    auto& allocator               = document.GetAllocator();

    for (rapidjson::Value const& hit : hits)
    {
        // Create an object to write to meta
        rapidjson::Value metaIndexObject(rapidjson::kObjectType);

        metaIndexObject.AddMember(
            "_type",
            rapidjson::Value().SetString(hit["_type"].GetString(), allocator),
            allocator);

        metaIndexObject.AddMember(
            "_id",
            rapidjson::Value().SetString(hit["_id"].GetString(), allocator),
            allocator);

        rapidjson::Value metaObject(rapidjson::kObjectType);

        metaObject.AddMember(
            "index",
            metaIndexObject,
            allocator);

        rapidjson::Writer<rapidjson::FileWriteStream> meta(stream);
        metaObject.Accept(meta);
        stream.Put('\n');

        // Write the _source object
        rapidjson::Writer<rapidjson::FileWriteStream> source(stream);
        hit["_source"].Accept(source);
        stream.Put('\n');
    }

    *scroll_id  = scroll_id_value.GetString();
    *hits_count = hits.Size();
}

void output_parser_error(
    rapidjson::Document const& doc,
    std::stringstream        & stream)
{
    stream << "JSON parsing failed with code: "
           << doc.GetParseError()
           << ", at offset "
           << doc.GetErrorOffset();
}

void dump(
    dump_options const& options,
    thread_state      * state)
{
    curl_wrap crl;

    std::string query = "{\n"
        "\"size\": " + std::to_string(options.size) + ",\n"
        "\"slice\": {\n"
            "\"id\": " + std::to_string(options.slice_id) + ",\n"
            "\"max\": " + std::to_string(options.slice_max) + "\n"
        "}\n"
    "}";

    curl_response resp;

    if (!crl.post_data(options.host + "/" + options.index + "/_search?scroll=1m", query, resp))
    {
        state->error << "A HTTP error occured: " << resp.error;
        return;
    }

    if (resp.response_code != 200)
    {
        state->error << "Server returned HTTP status " << resp.response_code;
        return;
    }

    rapidjson::Document doc;
    doc.Parse(resp.data ,resp.data_size);

    if (doc.HasParseError())
    {
        return output_parser_error(doc, state->error);
    }

    std::string scroll_id;
    int         hits_count;

    write_document(
        doc,
        &hits_count,
        &scroll_id);

    do
    {
        query = "{\n"
            "\"scroll\": \"1m\",\n"
            "\"scroll_id\": \"" + scroll_id + "\"\n"
        "}\n";

        curl_response resp_scroll;

        if (!crl.post_data(options.host + "/_search/scroll", query, resp_scroll))
        {
            state->error << "A HTTP error occured: " << resp_scroll.error;
            return;
        }

        if (resp.response_code != 200)
        {
            state->error << "Server returned HTTP status " << resp_scroll.response_code;
            return;
        }

        rapidjson::Document doc_search;
        doc_search.Parse(resp_scroll.data, resp_scroll.data_size);

        if (doc_search.HasParseError())
        {
            return output_parser_error(doc_search, state->error);
        }

        write_document(
            doc_search,
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
    std::vector<std::unique_ptr<thread_container>> threads;

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

    int num_size   = std::atoi(size.c_str());
    int num_slices = std::atoi(slices.c_str());

    for (int i = 0; i < num_slices; i++)
    {
        dump_options opts;
        opts.host      = host;
        opts.index     = index;
        opts.size      = num_size;
        opts.slice_id  = i;
        opts.slice_max = num_slices;

        auto cnt       = std::unique_ptr<thread_container>(new thread_container());
        cnt->slice_id  = i;
        cnt->thread    = std::thread(dump, opts, &cnt->state);

        threads.push_back(std::move(cnt));
    }

    int exit_code = 0;

    for (auto& cnt : threads)
    {
        cnt->thread.join();

        if (cnt->state.error.tellp() > 0)
        {
            std::cerr << "Slice "
                      << std::setw(2) << std::setfill('0') << cnt->slice_id
                      << " exited with error: "
                      << cnt->state.error.rdbuf()
                      << std::endl;

            exit_code = 1;
        }
    }

    curl_global_cleanup();

    return exit_code;
}

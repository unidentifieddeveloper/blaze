#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "argh.h"
#include "../vendor/rapidjson/include/rapidjson/document.h"
#include "../vendor/rapidjson/include/rapidjson/filewritestream.h"
#include "../vendor/rapidjson/include/rapidjson/writer.h"

#define DEFAULT_SIZE   5000
#define DEFAULT_SLICES 5
#define WRITE_BUF_SIZE 65536

static std::mutex mtx_out;

struct auth_options
{
    std::string type;
    std::string user;
    std::string pass;
    bool insecure;
};

struct dump_options
{
    std::string  host;
    std::string  index;
    auth_options auth;
    int          slice_id;
    int          slice_max;
    int          size;
};

struct thread_state
{
    std::stringstream error;
};

struct thread_container
{
    int          slice_id;
    thread_state state;
    std::thread  thread;
};

size_t write_data(
    void   * buffer,
    size_t   size,
    size_t   nmemb,
    void   * userp)
{
    std::vector<char>* data = reinterpret_cast<std::vector<char>*>(userp);

    const char* real_buffer = reinterpret_cast<const char*>(buffer);
    size_t real_size = size * nmemb;
    data->insert(data->end(), real_buffer, real_buffer + real_size);
    return real_size;
}

bool get_or_post_data(
    CURL                * crl,
    std::string   const & url,
    auth_options  const & auth,
    std::vector<char>   * data,
    long                * response_code,
    std::string         * error,
    std::string           body = "")
{
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(crl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(crl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(crl, CURLOPT_WRITEFUNCTION, &write_data);
    curl_easy_setopt(crl, CURLOPT_WRITEDATA,     reinterpret_cast<void*>(data));

    if (auth.insecure)
    {
        curl_easy_setopt(crl, CURLOPT_SSL_VERIFYPEER, 0);
        curl_easy_setopt(crl, CURLOPT_SSL_VERIFYHOST, 0);
    }

    if (auth.type == "basic")
    {
        std::string user_pass = auth.user + ":" + auth.pass;
        curl_easy_setopt(crl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(crl, CURLOPT_USERPWD,  user_pass.c_str());
    }

    if (!body.empty())
    {
        curl_easy_setopt(crl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode res = curl_easy_perform(crl);
    curl_slist_free_all(headers);

    if (res == CURLE_OK)
    {
        curl_easy_getinfo(crl, CURLINFO_RESPONSE_CODE, response_code);
        return true;
    }

    *error = curl_easy_strerror(res);
    return false;
}

void write_document(
    rapidjson::Document & document,
    int                 * hits_count,
    std::string         * scroll_id)
{
    std::unique_lock<std::mutex>      lock(mtx_out);

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
    auto  writer                  = rapidjson::Writer<rapidjson::FileWriteStream>(stream);

    for (rapidjson::Value const& hit : hits)
    {
        auto meta_index      = rapidjson::Value(rapidjson::kObjectType);
        auto meta_index_id   = rapidjson::Value();
        auto meta_object     = rapidjson::Value(rapidjson::kObjectType);

        meta_index_id.SetString(hit["_id"].GetString(), allocator);

        meta_index.AddMember("_id",   meta_index_id,   allocator);

        meta_object.AddMember("index", meta_index, allocator);

        // Serialize to output stream. Do it in two steps to get
        // new-line separated JSON.

        meta_object.Accept(writer);
        stream.Put('\n');
        stream.Flush();
        writer.Reset(stream);

        hit["_source"].Accept(writer);
        stream.Put('\n');
        stream.Flush();
        writer.Reset(stream);
    }

    *scroll_id  = scroll_id_value.GetString();
    *hits_count = hits.Size();
}

void output_parser_error(
    rapidjson::Document const& doc,
    std::ostream             & stream)
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
    CURL* crl = curl_easy_init();

    std::string query = "{\n"
        "\"size\": " + std::to_string(options.size) + ",\n"
        "\"slice\": {\n"
            "\"id\": " + std::to_string(options.slice_id) + ",\n"
            "\"max\": " + std::to_string(options.slice_max) + "\n"
        "}\n"
    "}";

    std::vector<char> buffer;
    long              response_code;
    std::string       error;

    bool res = get_or_post_data(
        crl,
        options.host + "/" + options.index + "/_search?scroll=1m",
        options.auth,
        &buffer,
        &response_code,
        &error,
        query);

    if (!res)
    {
        state->error << "A HTTP error occured: " << error;
        return;
    }

    if (response_code != 200)
    {
        state->error << "Server returned HTTP status " << response_code << ": " << buffer.data();
        return;
    }

    rapidjson::Document doc;
    doc.Parse(buffer.data(), buffer.size());

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

        buffer.clear();

        res = get_or_post_data(
            crl,
            options.host + "/_search/scroll",
            options.auth,
            &buffer,
            &response_code,
            &error,
            query);

        if (!res)
        {
            state->error << "A HTTP error occured: " << error;
            return;
        }

        if (response_code != 200)
        {
            state->error << "Server returned HTTP status " << response_code;
            return;
        }

        rapidjson::Document doc_search;
        doc_search.Parse(buffer.data(), buffer.size());

        if (doc_search.HasParseError())
        {
            return output_parser_error(doc_search, state->error);
        }

        write_document(
            doc_search,
            &hits_count,
            &scroll_id);
    } while (hits_count > 0);

    curl_easy_cleanup(crl);
}

int64_t count_documents(
    std::string  const& host,
    std::string  const& index,
    auth_options const& auth)
{
    CURL                * crl = curl_easy_init();
    long                  response_code;
    rapidjson::Document   doc;
    std::string           url = host + "/" + index + "/_count";
    std::string           error;
    std::vector<char>     buffer;

    bool res = get_or_post_data(
        crl,
        url,
        auth,
        &buffer,
        &response_code,
        &error);

    if (!res)
    {
        std::cerr << "A HTTP error occured: " << error << std::endl;
        return -1;
    }

    doc.Parse(buffer.data(), buffer.size());

    if (doc.HasParseError())
    {
        output_parser_error(doc, std::cerr);
        return -1;
    }

    return doc["count"].GetInt64();
}

int dump_mappings(
    std::string  const& host,
    std::string  const& index,
    auth_options const& auth)
{
    static char                       write_buffer[WRITE_BUF_SIZE];
    static rapidjson::FileWriteStream stream(stdout, write_buffer, sizeof(write_buffer));

    CURL                            * crl = curl_easy_init();
    long                              response_code;
    rapidjson::Document               doc;
    std::string                       url = host + "/" + index + "/_mapping";
    std::string                       error;
    std::vector<char>                 buffer;

    bool res = get_or_post_data(
        crl,
        url,
        auth,
        &buffer,
        &response_code,
        &error);

    if (!res)
    {
        std::cerr << "A HTTP error occured: " << error << std::endl;
        return 1;
    }

    doc.Parse(buffer.data(), buffer.size());

    if (doc.HasParseError())
    {
        output_parser_error(doc, std::cerr);
        return 1;
    }

    rapidjson::Writer<rapidjson::FileWriteStream> writer(stream);
    doc[index.c_str()].Accept(writer);
    stream.Put('\n');
    stream.Flush();

    curl_easy_cleanup(crl);

    return 0;
}

int dump_index_info(
    std::string  const& host,
    std::string  const& index,
    auth_options const& auth)
{
    static char                       write_buffer[WRITE_BUF_SIZE];
    static rapidjson::FileWriteStream stream(stdout, write_buffer, sizeof(write_buffer));

    CURL                            * crl = curl_easy_init();
    long                              response_code;
    rapidjson::Document               doc;
    std::string                       url = host + "/" + index;
    std::string                       error;
    std::vector<char>                 buffer;

    bool res = get_or_post_data(
        crl,
        url,
        auth,
        &buffer,
        &response_code,
        &error);

    if (!res)
    {
        std::cerr << "A HTTP error occured: " << error << std::endl;
        return 1;
    }

    doc.Parse(buffer.data(), buffer.size());

    if (doc.HasParseError())
    {
        output_parser_error(doc, std::cerr);
        return 1;
    }

    rapidjson::Writer<rapidjson::FileWriteStream> writer(stream);
    doc[index.c_str()].Accept(writer);
    stream.Put('\n');
    stream.Flush();

    curl_easy_cleanup(crl);

    return 0;
}

int main(
    int    argc,
    char * argv[])
{
    curl_global_init(CURL_GLOBAL_ALL);

    std::vector<std::unique_ptr<thread_container>> threads;

    // Parse command line options
    argh::parser cmdl(argv);

    std::string host;
    if (!(cmdl({"--host"}) >> host))
    {
        std::cerr << "Must provide an Elasticsearch host (--host)" << std::endl;
        return 1;
    }

    std::string index;
    if (!(cmdl({"--index"}) >> index))
    {
        std::cerr << "Must provide an index (--index)" << std::endl;
        return 1;
    }

    auth_options auth;

    if (cmdl({"--auth"}) >> auth.type)
    {
        if (auth.type == "basic")
        {
            if (!(cmdl({"--basic-username"}) >> auth.user))
            {
                std::cerr << "Must provide --basic-username when passing --auth=basic" << std::endl;
                return 1;
            }

            if (!(cmdl({"--basic-password"}) >> auth.pass))
            {
                std::cerr << "Must provide --basic-password when passing --auth=basic" << std::endl;
                return 1;
            }
        }
    }

    auth.insecure = cmdl["--insecure"];

    if (cmdl["--dump-mappings"])
    {
        return dump_mappings(
            host,
            index,
            auth);
    }
    else if (cmdl["--dump-index-info"])
    {
        return dump_index_info(
            host,
            index,
            auth);
    }

    // Sanity check - see if we have any documents in the index at all.
    if (count_documents(host, index, auth) <= 0)
    {
        std::cerr << "Index is empty - no documents found" << std::endl;
        return 0;
    }

    int slices;
    cmdl({"--slices"}, DEFAULT_SLICES) >> slices;

    int size;
    cmdl({"--size"}, DEFAULT_SIZE) >> size;

    for (int i = 0; i < slices; i++)
    {
        dump_options opts;
        opts.host      = host;
        opts.index     = index;
        opts.auth      = auth;
        opts.size      = size;
        opts.slice_id  = i;
        opts.slice_max = slices;

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

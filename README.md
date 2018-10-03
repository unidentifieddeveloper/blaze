# Blaze

Are you running Elasticsearch? Want to take your data and get the heck outta
Dodge? **Blaze** provides everything you need in a neat, blazing fast package!

| **Linux / OSX** |
| --------------- |
| [![Build Status](https://travis-ci.org/vktr/blaze.svg?branch=master)](https://travis-ci.org/vktr/blaze) |


## Features

 - Uses the [Elasticsearch sliced scroll API](https://www.elastic.co/guide/en/elasticsearch/reference/current/search-request-scroll.html) to get your data hella fast.
 - Written in modern C++ using [libcurl](https://github.com/curl/curl) and [RapidJSON](https://github.com/Tencent/RapidJSON).
 - Distributed as a single, tiny binary.


 ### Performance

Blaze compared to other Elasticsearch dump tools. The index has ~3.5M rows and
is ~5GB in size. Each tool is timed with `time` and measures the time to write
a simple JSON dump file.

| **Tool**    | **Time** |
| ----------- | -------- |
| Blaze       | 00m40s   |
| elasticdump | 04m38s   |


## Usage

Get the binary for your platform from the Releases page or compile it yourself.
If you use it often it might make sense to put it in your `PATH` somewhere.

```sh
$ blaze --host=http://localhost:9200 --index=massive_1 > dump.json
```

This will connect to Elasticsearch on the specified host and start downloading
the `massive_1` index to *stdout*. Make sure to redirect this somewhere, such as
a JSON file.


### Output format

Blaze will dump everything to *stdout* in a format compatible with the
Elasticsearch Bulk API, meaning you can use `curl` to put the data back.

```sh
curl -H "Content-Type: application/x-ndjson" -XPOST localhost:9200/other_data/_bulk --data-binary "@dump.json"
```

One issue when working with large datasets is that Elasticsearch has an upper
limit on the size of HTTP requests (2GB). The solution is to split the file
with something like `parallel`. The split should be done on even line numbers
since each command is actually two lines in the file.

```sh
cat dump.json | parallel --pipe -l 50000 curl -s -H "Content-Type: application/x-ndjson" -XPOST localhost:9200/other_data/_bulk --data-binary "@-"
```


### Command line options

 - `--host=<value>` - the host where Elasticsearch is running.
 - `--index=<value>` - the index to dump.
 - `--slices=<value>` - *(optional)* the number of slices to split the scroll. Should be set to the
   number of shards for the index (as seen on `/_cat/indices`). Defaults to *5*.
 - `--size=<value>` - *(optional)* the size of the response (i.e, length of the `hits` array).
   Defaults to *5000*.
 - `--dump-mappings` - specify this flag to dump the index mappings instead of the source.

#### Authentication

To use HTTP Basic authentication you need to pass the following options. *Note*
that passing a password on the command line will put it in your terminal
history, so please use with care.

 - `--auth=basic` - enable HTTP Basic authentication.
 - `--basic-username=foo` - the username.
 - `--basic-password=bar` - the password.


## Building from source

Building Blaze is easy. It requires `libcurl`.

### On Linux (and OSX)

```sh
$ git submodule update --init
$ make
```


## License

Copyright © Viktor Elofsson and contributors.

Blaze is provided as-is under the MIT license. For more information see
[LICENSE](https://github.com/vktr/blaze/blob/master/LICENSE).

 - For libcurl, see https://curl.haxx.se/docs/copyright.html 
 - For RapidJSON, see https://github.com/Tencent/rapidjson/blob/master/license.txt

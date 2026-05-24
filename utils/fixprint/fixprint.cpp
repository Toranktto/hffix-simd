#include <hffix.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>

char const color_field[] = "\x1b[33m";
char const color_value[] = "\x1b[37m";
char const color_msgtype[] = "\x1b[32m";
char const color_default[] = "\x1b[39m";

enum { chunksize = 4096 };

char buffer[1 << 20];

int main(int argc, char** argv) {
    if (argc > 1 && ((0 == std::strcmp("-h", argv[1])) || (0 == std::strcmp("--help", argv[1])))) {
        std::cout << "fixprint [Options]\n\n"
                     "Reads raw FIX encoded data from stdin and writes annotated human-readable "
                     "FIX to stdout.\n\n"
                     "Options:\n"
                     "  -c --color     Color output.\n"
                     "     --no-color  No color output.\n\n";
        exit(0);
    }

    bool color = true;
    if (argc > 1) {
        if (0 == std::strcmp("-c", argv[1]))
            color = true;
        if (0 == std::strcmp("--color", argv[1]))
            color = true;
        if (0 == std::strcmp("--no-color", argv[1]))
            color = false;
    }

    std::map<int, std::string> field_dictionary;
    hffix::dictionary_init_field(field_dictionary);
    std::map<std::string, std::string> message_dictionary;
    hffix::dictionary_init_message(message_dictionary);

    size_t buffer_length(0);
    size_t fred;

    while ((fred = std::fread(buffer + buffer_length,
                              1,
                              std::min(sizeof(buffer) - buffer_length, size_t(chunksize)),
                              stdin))) {
        buffer_length += fred;
        hffix::message_reader reader(buffer, buffer + buffer_length);

        for (; reader.is_complete(); reader = reader.next_message_reader()) {
            if (reader.is_valid()) {
                if (color)
                    std::cout << color_value;
                std::cout.write(reader.prefix_begin(), reader.prefix_size());
                std::cout << ' ';

                try {
                    for (hffix::message_reader::const_iterator i = reader.begin(); i != reader.end();
                         ++i) {
                        if (color)
                            std::cout << color_field;

                        std::map<int, std::string>::iterator fname = field_dictionary.find(i->tag());
                        if (fname != field_dictionary.end())
                            std::cout << fname->second << '_';
                        std::cout << i->tag();

                        std::cout << '=';

                        if (color)
                            std::cout
                                << (i->tag() == hffix::tag::MsgType ? color_msgtype : color_value);

                        std::cout.write(i->value().begin(), i->value().size());

                        if (i->tag() == hffix::tag::MsgType) {
                            std::map<std::string, std::string>::iterator mname =
                                message_dictionary.find(
                                    std::string(i->value().begin(), i->value().end()));
                            if (mname != message_dictionary.end())
                                std::cout << '_' << mname->second;
                        }

                        std::cout << ' ';
                    }

                    std::cout << '\n';
                } catch (std::exception& ex) {
                    std::cerr << "Error reading the fields: " << ex.what() << '\n';
                }

            } else {
                std::cerr << "Error Corrupt FIX message: ";
                std::cerr.write(
                    reader.message_begin(),
                    std::min(ssize_t(64), buffer + buffer_length - reader.message_begin()));
                std::cerr << "...\n";
            }
        }
        buffer_length = reader.buffer_end() - reader.buffer_begin();

        if (buffer_length > 0)
            std::memmove(buffer, reader.buffer_begin(), buffer_length);
    }

    if (color)
        std::cout << color_default;
    return 0;
}

#include "json.hh"

#include "exception.hh"
#include "string_utils.hh"
#include "unit_tests.hh"
#include "utils.hh"
#include "ranges.hh"
#include "utf8.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace Kakoune
{

String to_json(int i) { return to_string(i); }
String to_json(bool b) { return b ? "true" : "false"; }
String to_json(double d)
{
    char buf[32];
    // %.17g round-trips an IEEE-754 double exactly.
    int len = snprintf(buf, sizeof(buf), "%.17g", d);
    return String{buf, ByteCount{len}};
}

String to_json(StringView str)
{
    String res;
    res.reserve(str.length() + 4);
    res += '"';
    for (auto it = str.begin(), end = str.end(); it != end; )
    {
        auto next = std::find_if(it, end, [](char c) {
            return c == '\\' or c == '"' or (unsigned char) c <= 0x1F;
        });

        res += StringView{it, next};
        if (next == end)
            break;

        char buf[7] = {'\\', *next, 0};
        if ((unsigned char) *next <= 0x1F)
            format_to(buf, "\\u{:04}", hex(*next));

        res += buf;
        it = next+1;
    }
    res += '"';
    return res;
}

template<typename T> requires std::is_same_v<T, Value>
String to_json(const T& value)
{
    if (not value)
        return "null";
    if (value.template is_a<bool>())
        return to_json(value.template as<bool>());
    if (value.template is_a<int>())
        return to_json(value.template as<int>());
    if (value.template is_a<double>())
        return to_json(value.template as<double>());
    if (value.template is_a<String>())
        return to_json(StringView{value.template as<String>()});
    if (value.template is_a<JsonArray>())
    {
        // Serialize element-wise via const references: Value is move-only with a
        // greedy templated converting constructor, so routing it through the
        // generic range-based to_json overloads would copy lvalue Values into
        // nested Value-in-Value models and recurse forever.
        auto& array = value.template as<JsonArray>();
        String res = "[";
        bool first = true;
        for (auto&& elem : array)
        {
            if (not std::exchange(first, false))
                res += ", ";
            res += to_json(elem);
        }
        res += "]";
        return res;
    }
    if (value.template is_a<JsonObject>())
    {
        auto& object = value.template as<JsonObject>();
        String res = "{";
        bool first = true;
        for (auto&& item : object)
        {
            if (not std::exchange(first, false))
                res += ',';
            res += to_json(StringView{item.key});
            res += ": ";
            res += to_json(item.value);
        }
        res += "}";
        return res;
    }
    throw runtime_error("cannot serialize value to json");
}
// Explicit instantiation so the symbol is emitted for callers in other TUs.
template String to_json<Value>(const Value&);

static bool is_digit(char c) { return c >= '0' and c <= '9'; }

static double str_to_double(StringView str)
{
    return strtod(String{str}.c_str(), nullptr);
}

static int hex_digit(char c)
{
    if (c >= '0' and c <= '9') return c - '0';
    if (c >= 'a' and c <= 'f') return c - 'a' + 10;
    if (c >= 'A' and c <= 'F') return c - 'A' + 10;
    return -1;
}

static constexpr size_t max_parsing_depth = 100;

JsonResult parse_json_impl(const char* pos, const char* end, size_t depth)
{
    if (not skip_while(pos, end, is_blank))
        return {};

    if (depth >= max_parsing_depth)
        throw runtime_error("maximum parsing depth reached");

    if (is_digit(*pos) or *pos == '-')
    {
        auto num_end = pos + 1;
        skip_while(num_end, end, is_digit);
        bool is_float = false;
        if (num_end != end and *num_end == '.')
        {
            is_float = true;
            skip_while(++num_end, end, is_digit);
        }
        if (num_end != end and (*num_end == 'e' or *num_end == 'E'))
        {
            is_float = true;
            if (++num_end != end and (*num_end == '+' or *num_end == '-'))
                ++num_end;
            skip_while(num_end, end, is_digit);
        }
        if (is_float)
            return { Value{str_to_double({pos, num_end})}, num_end };
        return { Value{str_to_int({pos, num_end})}, num_end };
    }
    if (end - pos >= 4 and StringView{pos, pos+4} == "null")
        return { Value{}, pos+4 };
    if (end - pos >= 4 and StringView{pos, pos+4} == "true")
        return { Value{true}, pos+4 };
    if (end - pos >= 5 and StringView{pos, pos+5} == "false")
        return { Value{false}, pos+5 };
    if (*pos == '"')
    {
        ++pos;
        String value;
        while (pos != end)
        {
            char c = *pos;
            if (c == '"')
                return { std::move(value), pos + 1 };
            if (c != '\\')
            {
                value.push_back(c);
                ++pos;
                continue;
            }
            // escape sequence
            if (++pos == end)
                return {}; // incomplete
            switch (*pos)
            {
                case '"':  value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/':  value.push_back('/'); break;
                case 'b':  value.push_back('\b'); break;
                case 'f':  value.push_back('\f'); break;
                case 'n':  value.push_back('\n'); break;
                case 'r':  value.push_back('\r'); break;
                case 't':  value.push_back('\t'); break;
                case 'u':
                {
                    auto read4 = [](const char* p) {
                        int v = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            int h = hex_digit(p[i]);
                            if (h < 0) return -1;
                            v = (v << 4) | h;
                        }
                        return v;
                    };
                    if (end - pos < 5) // 'u' + 4 hex digits
                        return {};
                    int cp = read4(pos + 1);
                    if (cp < 0)
                        throw runtime_error("invalid \\u escape in json string");
                    pos += 4; // now at the last hex digit
                    if (cp >= 0xD800 and cp <= 0xDBFF and end - pos >= 7 and
                        *(pos + 1) == '\\' and *(pos + 2) == 'u')
                    {
                        int lo = read4(pos + 3);
                        if (lo >= 0xDC00 and lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos += 6;
                        }
                    }
                    utf8::dump(std::back_inserter(value), (Codepoint)cp);
                    break;
                }
                default: value.push_back(*pos); break; // unknown escape: keep char
            }
            ++pos;
        }
        return {}; // unterminated string
    }
    if (*pos == '[')
    {
        JsonArray array;
        if (++pos == end)
            throw runtime_error("unable to parse array");
        if (*pos == ']')
            return {std::move(array), pos+1};

        while (true)
        {
            auto [element, new_pos] = parse_json_impl(pos, end, depth+1);
            if (not new_pos) // incomplete input (a parsed json null has a valid new_pos)
                return {};
            pos = new_pos;
            array.push_back(std::move(element));
            if (not skip_while(pos, end, is_blank))
                return {};

            if (*pos == ',')
                ++pos;
            else if (*pos == ']')
                return {std::move(array), pos+1};
            else
                throw runtime_error("unable to parse array, expected ',' or ']'");
        }
    }
    if (*pos == '{')
    {
        if (++pos == end)
            throw runtime_error("unable to parse object");
        JsonObject object;
        if (*pos == '}')
            return {std::move(object), pos+1};

        while (true)
        {
            auto [name_value, name_end] = parse_json_impl(pos, end, depth+1);
            if (not name_end) // incomplete input
                return {};
            pos = name_end;
            String& name = name_value.as<String>();
            if (not skip_while(pos, end, is_blank))
                return {};
            if (*pos++ != ':')
                throw runtime_error("expected :");

            auto [element, element_end] = parse_json_impl(pos, end, depth+1);
            if (not element_end) // incomplete input (a parsed json null has a valid new_pos)
                return {};
            pos = element_end;
            object.insert({ std::move(name), std::move(element) });
            if (not skip_while(pos, end, is_blank))
                return {};

            if (*pos == ',')
                ++pos;
            else if (*pos == '}')
                return {std::move(object), pos+1};
            else
                throw runtime_error("unable to parse object, expected ',' or '}'");
        }
    }
    throw runtime_error("unable to parse json");
}

JsonResult parse_json(const char* pos, const char* end) { return parse_json_impl(pos,          end,        0); }
JsonResult parse_json(StringView json)                  { return parse_json_impl(json.begin(), json.end(), 0); }

UnitTest test_json_parser{[]()
{
    {
        auto value = parse_json(R"({ "jsonrpc": "2.0", "method": "keys", "params": [ "b", "l", "a", "h" ] })").value;
        kak_assert(value);
    }

    {
        auto value = parse_json("[10,20]").value;
        kak_assert(value and value.is_a<JsonArray>());
        kak_assert(value.as<JsonArray>().at(1).as<int>() == 20);
    }

    {
        auto value = parse_json("-1").value;
        kak_assert(value.as<int>() == -1);
    }

    {
        auto value = parse_json("{}").value;
        kak_assert(value and value.is_a<JsonObject>());
        kak_assert(value.as<JsonObject>().empty());
    }

    {
        char big_nested_array[max_parsing_depth*2+2+1] = {};
        for (size_t i = 0; i < max_parsing_depth+1; i++)
        {
            big_nested_array[i] = '[';
            big_nested_array[i+max_parsing_depth+1] = ']';
        }
        kak_expect_throw(runtime_error, parse_json(big_nested_array));
    }

    {
        // null parses to an empty Value, distinct from a parse failure.
        auto res = parse_json("null");
        kak_assert(res.new_pos != nullptr and not res.value);
    }

    {
        // a null inside a container must not abort parsing of the container.
        auto value = parse_json(R"([1, null, 3])").value;
        kak_assert(value and value.is_a<JsonArray>());
        auto& arr = value.as<JsonArray>();
        kak_assert(arr.size() == 3 and arr[0].as<int>() == 1 and not arr[1] and arr[2].as<int>() == 3);
    }

    {
        auto value = parse_json(R"({ "a": null, "b": 2 })").value;
        kak_assert(value and value.is_a<JsonObject>());
        auto& obj = value.as<JsonObject>();
        auto a = obj.find("a"_sv);
        auto b = obj.find("b"_sv);
        kak_assert(a != obj.end() and not a->value);
        kak_assert(b != obj.end() and b->value.as<int>() == 2);
    }

    {
        kak_assert(parse_json("1.5").value.as<double>() == 1.5);
        kak_assert(parse_json("-2.25").value.as<double>() == -2.25);
        kak_assert(parse_json("6e2").value.as<double>() == 600.0);
        kak_assert(parse_json("1.5e-1").value.as<double>() == 0.15);
        // integers stay ints
        kak_assert(parse_json("42").value.as<int>() == 42);
    }

    {
        // string escape decoding (previously the parser just dropped the backslash)
        kak_assert(parse_json(R"("a\nb")").value.as<String>() == "a\nb");
        kak_assert(parse_json(R"("x\ty\r\\z\/w\"q")").value.as<String>() == "x\ty\r\\z/w\"q");
        kak_assert(parse_json(R"("\u0041\u0042")").value.as<String>() == "AB"); // \u decode
        kak_assert(parse_json(R"("é")").value.as<String>().length() == 2); // U+00E9 -> 2 UTF-8 bytes
        kak_assert(parse_json(R"("😀")").value.as<String>().length() == 4); // surrogate pair -> 4 bytes
    }
}};

UnitTest test_to_json{[]()
{
    kak_assert(to_json(Value{}) == "null");
    kak_assert(to_json(Value{42}) == "42");
    kak_assert(to_json(Value{String{"hi"}}) == "\"hi\"");
    {
        JsonArray arr;
        arr.push_back(Value{1});
        arr.push_back(Value{});
        arr.push_back(Value{String{"x"}});
        kak_assert(to_json(Value{std::move(arr)}) == R"([1, null, "x"])");
    }
    // round-trip a parsed nested tree through serialization
    kak_assert(to_json(parse_json(R"([1, null, "x"])").value) == R"([1, null, "x"])");
    // single key is deterministic; HashMap does not preserve insertion order
    kak_assert(to_json(parse_json(R"({"a": [1, 2]})").value) == R"({"a": [1, 2]})");
    {
        // multi-key object: re-parse the serialization and check structure rather
        // than rely on key ordering.
        auto reparsed = parse_json(to_json(parse_json(R"({"a": 1, "b": null})").value)).value;
        kak_assert(reparsed.is_a<JsonObject>());
        auto& obj = reparsed.as<JsonObject>();
        auto a = obj.find("a"_sv);
        auto b = obj.find("b"_sv);
        kak_assert(a != obj.end() and a->value.as<int>() == 1);
        kak_assert(b != obj.end() and not b->value);
    }
    kak_assert(to_json(true) == "true");
    kak_assert(to_json(false) == "false");
    kak_assert(to_json(HashMap<String, Vector<int>>{{"foo", {1,2,3}}, {"\033", {3, 4, 5}}}) == R"({"foo": [1, 2, 3],"\u001b": [3, 4, 5]})");
}};

}

// This tests isolated pieces of code that don't need interaction with a real HTTP server.
#define _CRT_SECURE_NO_WARNINGS
#include "http-bridge.h"
#include <stdio.h>

#ifdef assert
#undef assert
#endif

// The second BREAKME version is useful when debugging
//#define BREAKME (void)0
#ifndef BREAKME
#	ifdef _MSC_VER
#		define BREAKME __debugbreak()
#	else
#		define BREAKME __builtin_trap()
#	endif
#endif

#define assert(exp) (void) ((exp) || (printf("Error: %s:%d:\n %s\n", __FILE__, __LINE__, #exp), BREAKME, exit(1), 0))
#define run(f) (printf("%-30s\n", #f), f())

bool streq(const char* a, const char* b) { return strcmp(a, b) == 0; }

void* AllocOrDie(size_t size)
{
	void* b = malloc(size);
	assert(b != nullptr);
	return b;
}

void TestUrlQueryParser()
{
#define checkparse_points(_k,_kl,_v,_vl) assert(p.Next(k, kl, v, vl) && k == _k && kl == _kl && v == _v && vl == _vl)
#define checkparse_content(_key, _val) p.DecodeKey(k, kbuf, true); p.DecodeVal(v, vbuf, true); assert(strcmp(kbuf, _key) == 0 && (strcmp(vbuf, _val) == 0 || vl == 0))
#define end() assert(!p.Next(k, kl, v, vl))

	int k, kl, v, vl;
	char kbuf[100], vbuf[100];
	{
		hb::UrlQueryParser p("a");
		checkparse_points(0, 1, -1, 0);
		checkparse_content("a", "");
		end();
	}
	{
		hb::UrlQueryParser p("a=b");
		checkparse_points(0, 1, 2, 1);
		checkparse_content("a", "b");
		end();
	}
	{
		hb::UrlQueryParser p("a==b");
		checkparse_points(0, 1, 2, 2);
		checkparse_content("a", "=b");
		end();
	}
	{
		hb::UrlQueryParser p("a&b");
		checkparse_points(0, 1, -1, 0);
		checkparse_content("a", "");
		checkparse_points(2, 1, -1, 0);
		checkparse_content("b", "");
		end();
	}
	// %25 %
	// %26 &
	// %3d =
	{
		hb::UrlQueryParser p("a%26b=c");
		checkparse_points(0, 3, 6, 1);
		checkparse_content("a&b", "c");
		end();
	}
	{
		hb::UrlQueryParser p("%3d=%3D");
		checkparse_points(0, 1, 4, 1);
		checkparse_content("=", "=");
		end();
	}
	{
		hb::UrlQueryParser p("%zz");
		checkparse_points(0, 1, -1, 0);
		checkparse_content("", "");
		end();
	}
	{
		hb::UrlQueryParser p("%=z");
		checkparse_points(0, 1, -1, 0);
		checkparse_content("", "");
		end();
	}
	{
		hb::UrlQueryParser p("%25=%25&x=yz");
		checkparse_points(0, 1, 4, 1);
		checkparse_content("%", "%");
		checkparse_points(8, 1, 10, 2);
		checkparse_content("x", "yz");
		end();
	}
	{
		// unfinished %aa
		hb::UrlQueryParser p("%a");
		checkparse_points(0, 2, -1, 0);
		checkparse_content("%a", "");
		end();
	}
	{
		hb::UrlQueryParser p("a+b");
		checkparse_points(0, 3, -1, 0);
		checkparse_content("a b", "");
		end();
	}


#undef checkparse_points
#undef end
}

void SetupRequest(hb::Request& r, hb::Backend& back, const char* uri)
{
	char* hblock = (char*) hb::Alloc(8 + strlen(uri) + 1, nullptr);
	hb::Request::HeaderLine* lines = (hb::Request::HeaderLine*) hblock;
	lines[0].KeyStart = 7;
	lines[0].KeyLen = 0;
	strcpy(hblock + 8, uri);
	r.Initialize(&back, hb::HttpVersion11, 0, 0, 1, hblock);
}

void TestRequestQuerySplitter()
{
	auto test = [](const char* uri, const char* path, std::vector<std::pair<std::string, std::string>> pairs)
	{
		hb::Backend back;
		hb::Request r;
		SetupRequest(r, back, uri);
		assert(r.ParseURI());
		assert(strcmp(r.Path(), path) == 0);
		assert(r.Query("i_don't_exist") == nullptr);
		int32_t iter = 0;
		const char* key, *val;
		for (auto p : pairs)
		{
			iter = r.NextQuery(iter, key, val);
			assert(p.first == key);
			assert(p.second == val);
			assert(p.second == r.Query(p.first.c_str()));
		}
		assert(r.NextQuery(iter, key, val) == 0);
	};
	test("", "", {});
	test("/a/path", "/a/path", {});
	test("?a=b", "", { { "a", "b" } });
	test("?a=", "", { { "a", "" } });
	test("?a=&b=&c", "", { { "a", "" }, { "b", "" }, { "c", "" } });
	test("?", "", {});
	test("?h", "", { { "h", "" } });
	test("?hello", "", { { "hello", "" } });
	test("PATH?a=b", "PATH", { { "a", "b" } });
	test("PATH???a=b", "PATH", { { "??a", "b" } });
	test("%00%01%02?%00=%01", "\x00\x01\x02", { { "\x00", "\x01" } });

	// test different combinations of lengths of key and value.
	// When vlen = 1, then don't emit an equals sign.
	for (int klen = 1; klen <= 5; klen++)
	{
		for (int vlen = -1; vlen <= 5; vlen++)
		{
			std::string q = "?", k, v;
			for (int i = 0; i < klen; i++)
				k += '1' + i;
			for (int i = 0; i < vlen; i++)
				v += 'a' + i;
			
			if (vlen == -1)
				q += k;
			else
				q += k + "=" + v;
			test(q.c_str(), "", { { k, v } });
		}
	}

	auto expect_fail = [](const char* uri)
	{
		hb::Request r;
		hb::Backend back;
		SetupRequest(r, back, uri);
		assert(!r.ParseURI());
	};

	{
		// longest possible path
		char* buf = (char*) AllocOrDie(65534 + 1);
		memset(buf, 1, 65534);
		buf[65534] = 0;
		test(buf, buf, {});
		free(buf);
	}
	{
		// path too long
		char* buf = (char*) AllocOrDie(65535 + 1);
		memset(buf, 1, 65535);
		buf[65535] = 0;
		expect_fail(buf);
		free(buf);
	}
	{
		// query too long
		char* buf = (char*) AllocOrDie(65536 + 1);
		memset(buf, 1, 65536);
		buf[0] = '?';
		buf[65536] = 0;
		expect_fail(buf);
		free(buf);
	}

	{
		hb::Request r;
		hb::Backend back;
		SetupRequest(r, back, "?x=1122334455667788&y=1.5");
		assert(r.ParseURI());
		assert(r.QueryInt64("x") == (int64_t) 1122334455667788ll);
		assert(r.QueryDouble("y") == 1.5);
	}
}

void TestResponseMisc()
{
	hb::Response r;
	assert(r.HeaderCount() == 0);
	r.AddHeader("abc", "1234");
	assert(r.HeaderCount() == 1);
	assert(streq(r.HeaderByName("abc"), "1234"));
	assert(r.HasHeader("abc"));
	int32_t keyLen = 0;
	int32_t valLen = 0;
	const char* key = nullptr;
	const char* val = nullptr;
	r.HeaderAt(0, keyLen, key, valLen, val);
	assert(keyLen == 3);
	assert(valLen == 4);
	assert(streq(key, "abc"));
	assert(streq(val, "1234"));

	// out of range
	r.HeaderAt(1, keyLen, key, valLen, val);
	assert(key == nullptr);
	assert(val == nullptr);
	assert(keyLen == 0);
	assert(valLen == 0);
	assert(r.HeaderByName("a") == nullptr);
	assert(!r.HasHeader("a"));
}

void TestUtilFunctions()
{
	char buf[100];
	hb::U64toa(123456789012345ull, buf, sizeof(buf));
	assert(streq(buf, "123456789012345"));

	hb::U64toa(0, buf, sizeof(buf));
	assert(streq(buf, "0"));

	// buffer too small. emits back two digits, plus null terminator
	hb::U64toa(123456789012345ull, buf, 3);
	assert(streq(buf, "45"));

	assert( hb::uatoi64("123456789012345", 15) == 123456789012345ull);
}

int main(int argc, char** argv)
{
	run(TestUrlQueryParser);
	run(TestRequestQuerySplitter);
	run(TestResponseMisc);
	run(TestUtilFunctions);
	return 0;
}
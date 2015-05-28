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


#undef checkparse_points
#undef end
}

void SetupRequest(hb::Request& r, hb::Backend& back, const char* uri)
{
	char* hblock = (char*) malloc(8 + strlen(uri) + 1);
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
		hb::Request r;
		hb::Backend back;
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
	test("PATH?a=b", "PATH", { { "a", "b" } });
	test("PATH???a=b", "PATH", { { "??a", "b" } });
	test("%00%01%02?%00=%01", "\x00\x01\x02", { { "\x00", "\x01" } });

	auto expect_fail = [](const char* uri)
	{
		hb::Request r;
		hb::Backend back;
		SetupRequest(r, back, uri);
		assert(!r.ParseURI());
	};

	{
		// longest possible path
		char* buf = (char*) malloc(65534 + 1);
		memset(buf, 1, 65534);
		buf[65534] = 0;
		test(buf, buf, {});
		free(buf);
	}
	{
		// path too long
		char* buf = (char*) malloc(65535 + 1);
		memset(buf, 1, 65535);
		buf[65535] = 0;
		expect_fail(buf);
		free(buf);
	}
	{
		// query too long
		char* buf = (char*) malloc(65536 + 1);
		memset(buf, 1, 65536);
		buf[0] = '?';
		buf[65536] = 0;
		expect_fail(buf);
		free(buf);
	}
}

int main(int argc, char** argv)
{
	run(TestUrlQueryParser);
	run(TestRequestQuerySplitter);
	return 0;
}
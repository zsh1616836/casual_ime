#ifndef TOOLS_H
#define TOOLS_H

#include <cstdlib>
#include <stdint.h>

class tools
{
public:
	static int64_t str_to_long(const char* str)
	{
		char* end;
		const int64_t val = strtoll(str, &end, 10);
		return val;
	}

	static int bytes_to_int(const char* bytes)
	{
		return ((bytes[0] & 0xff) << 24) + ((bytes[1] & 0xff) << 16) + ((bytes[2] & 0xff) << 8) + (bytes[3] & 0xff);
	}

	static int intPow(int x, int y) {
		int result = 1;
		while (y > 0) {
			if (y & 1) {        // 如果 y 是奇数
				result *= x;
			}
			x *= x;             // 底数平方
			y >>= 1;            // 指数右移一位，相当于除以2
		}
		return result;
	}

	static int get_code_value(const std::wstring& code)
	{
		int hash = 0;
		for (wchar_t ch : code)
		{
			int c = (char)ch - L'`'; // 'a'=1, 'b'=2, ..., 'y'=25
			if (c < 1 || c > 25)
				return -1;
			hash = hash * 26 + c;
		}
		return hash;
	}

	static int get_code_value(const std::string& code)
	{
		int hash = 0;
		for (char ch : code)
		{
			int c = ch - '`'; // 'a'=1, 'b'=2, ..., 'y'=25
			if (c < 1 || c > 25)
				return -1;
			hash = hash * 26 + c;
		}
		return hash;
	}
};

#endif

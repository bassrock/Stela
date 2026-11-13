// SPDX-License-Identifier: GPL-2.0-only
/* Parts are adapted from the Linux file named below, whose notice is:
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
/* Linux's formatted input, following Linux
 * cc9de5d4b5ab6459f5774946aa3195ad51236fa8 lib/vsprintf.c's vsscanf, with its
 * departures from C's: a '[' conversion takes no ranges, cannot match ']',
 * needs a field width and has no '*' form. */
#include <linux/bitmap.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/sprintf.h>
#include <linux/string.h>
/* lib/kstrtox.h, beside the include directory. */
#include <../lib/kstrtox.h>

static int skip_atoi(const char **s)
{
	int i = 0;

	do {
		i = i * 10 + *((*s)++) - '0';
	} while (isdigit(**s));
	return i;
}

static unsigned long long simple_strntoull(const char *startp, char **endp, unsigned int base,
					   size_t max_chars)
{
	const char *cp;
	unsigned long long result = 0ULL;
	size_t prefix_chars;
	unsigned int rv;

	cp = _parse_integer_fixup_radix(startp, &base);
	prefix_chars = (size_t)(cp - startp);
	if (prefix_chars < max_chars) {
		rv = _parse_integer_limit(cp, base, &result, max_chars - prefix_chars);
		/* FIXME */
		cp += (rv & ~KSTRTOX_OVERFLOW);
	} else {
		/* Field too short for prefix + digit, skip over without converting */
		cp = startp + max_chars;
	}
	if (endp)
		*endp = (char *)cp;
	return result;
}

/* Linux negates the unsigned result and converts it; so does this. */
static long long simple_strntoll(const char *cp, char **endp, unsigned int base, size_t max_chars)
{
	if (*cp == '-' && max_chars > 0)
		return (long long)(0ULL - simple_strntoull(cp + 1, endp, base, max_chars - 1));
	return (long long)simple_strntoull(cp, endp, base, max_chars);
}

int vsscanf(const char *buf, const char *fmt, va_list args)
{
	const char *str = buf;
	char *next;
	char digit;
	int num = 0;
	u8 qualifier;
	unsigned int base;
	union {
		long long s;
		unsigned long long u;
	} val;
	s16 field_width;
	bool is_sign;

	while (*fmt) {
		/* White space in the format matches any amount, including none. */
		if (isspace(*fmt)) {
			fmt = skip_spaces(++fmt);
			str = skip_spaces(str);
		}

		/* Anything that is not a conversion must match exactly. */
		if (*fmt != '%' && *fmt) {
			if (*fmt++ != *str++)
				break;
			continue;
		}

		if (!*fmt)
			break;
		++fmt;

		/* Skip this conversion: both strings to their next white space. */
		if (*fmt == '*') {
			if (!*str)
				break;
			while (!isspace(*fmt) && *fmt != '%' && *fmt) {
				/* '%*[' not yet supported, invalid format */
				if (*fmt == '[')
					return num;
				fmt++;
			}
			while (!isspace(*str) && *str)
				str++;
			continue;
		}

		field_width = -1;
		if (isdigit(*fmt)) {
			field_width = (s16)skip_atoi(&fmt);
			if (field_width <= 0)
				break;
		}

		qualifier = (u8)-1;
		if (*fmt == 'h' || _tolower(*fmt) == 'l' || *fmt == 'z') {
			qualifier = (u8)*fmt++;
			if (unlikely(qualifier == *fmt)) {
				if (qualifier == 'h') {
					qualifier = 'H';
					fmt++;
				} else if (qualifier == 'l') {
					qualifier = 'L';
					fmt++;
				}
			}
		}

		if (!*fmt)
			break;

		if (*fmt == 'n') {
			/* return number of characters read so far */
			*va_arg(args, int *) = (int)(str - buf);
			++fmt;
			continue;
		}

		if (!*str)
			break;

		base = 10;
		is_sign = false;

		switch (*fmt++) {
		case 'c': {
			char *s = va_arg(args, char *);

			if (field_width == -1)
				field_width = 1;
			do {
				*s++ = *str++;
			} while (--field_width > 0 && *str);
			num++;
		}
			continue;
		case 's': {
			char *s = va_arg(args, char *);

			if (field_width == -1)
				field_width = SHRT_MAX;
			/* first, skip leading white space in buffer */
			str = skip_spaces(str);
			/* now copy until next white space */
			while (*str && !isspace(*str) && field_width--)
				*s++ = *str++;
			*s = '\0';
			num++;
		}
			continue;
		case '[': {
			char *s = va_arg(args, char *);
			DECLARE_BITMAP(set, 256) = {0};
			unsigned int len = 0;
			bool negate = (*fmt == '^');

			/* field width is required */
			if (field_width == -1)
				return num;
			if (negate)
				++fmt;
			for (; *fmt && *fmt != ']'; ++fmt, ++len)
				__set_bit((u8)*fmt, set);
			/* no ']' or no character set found */
			if (!*fmt || !len)
				return num;
			++fmt;
			if (negate) {
				bitmap_complement(set, set, 256);
				/* exclude null '\0' byte */
				__clear_bit(0, set);
			}
			/* match must be non-empty */
			if (!test_bit((u8)*str, set))
				return num;
			while (test_bit((u8)*str, set) && field_width--)
				*s++ = *str++;
			*s = '\0';
			++num;
		}
			continue;
		case 'o':
			base = 8;
			break;
		case 'x':
		case 'X':
			base = 16;
			break;
		case 'i':
			base = 0;
			fallthrough;
		case 'd':
			is_sign = true;
			fallthrough;
		case 'u':
			break;
		case '%':
			/* looking for '%' in str */
			if (*str++ != '%')
				return num;
			continue;
		default:
			/* invalid format; stop here */
			return num;
		}

		/* An integer conversion: first, skip white space in buffer. */
		str = skip_spaces(str);
		digit = *str;
		if (is_sign && digit == '-') {
			if (field_width == 1)
				break;
			digit = *(str + 1);
		}

		if (!digit || (base == 16 && !isxdigit(digit)) || (base == 10 && !isdigit(digit)) ||
		    (base == 8 && !isodigit(digit)) || (base == 0 && !isdigit(digit)))
			break;

		size_t max_chars = field_width >= 0 ? (size_t)field_width : (size_t)INT_MAX;

		if (is_sign)
			val.s = simple_strntoll(str, &next, base, max_chars);
		else
			val.u = simple_strntoull(str, &next, base, max_chars);

		switch (qualifier) {
		case 'H': /* that's 'hh' in format */
			if (is_sign)
				*va_arg(args, signed char *) = (signed char)val.s;
			else
				*va_arg(args, unsigned char *) = (unsigned char)val.u;
			break;
		case 'h':
			if (is_sign)
				*va_arg(args, short *) = (short)val.s;
			else
				*va_arg(args, unsigned short *) = (unsigned short)val.u;
			break;
		case 'l':
			if (is_sign)
				*va_arg(args, long *) = (long)val.s;
			else
				*va_arg(args, unsigned long *) = (unsigned long)val.u;
			break;
		case 'L':
			if (is_sign)
				*va_arg(args, long long *) = val.s;
			else
				*va_arg(args, unsigned long long *) = val.u;
			break;
		case 'z':
			*va_arg(args, size_t *) = (size_t)val.u;
			break;
		default:
			if (is_sign)
				*va_arg(args, int *) = (int)val.s;
			else
				*va_arg(args, unsigned int *) = (unsigned int)val.u;
			break;
		}
		num++;

		if (!next)
			break;
		str = next;
	}

	return num;
}

int sscanf(const char *buf, const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	i = vsscanf(buf, fmt, args);
	va_end(args);
	return i;
}

#ifdef STELA_LINUX_VSSCANF_TEST
#include <stela/test.h>

int main(void)
{
	unsigned int se, sh, cu;
	char first[8], second[8];
	long value;
	int number, consumed;

	/* amdgpu's own formats: disable_cu's triples, a partition value and
	 * PTL's two format names. */
	assert(sscanf("1.2.3", "%u.%u.%u", &se, &sh, &cu) == 3 && se == 1 && sh == 2 && cu == 3);
	assert(sscanf("4.5", "%u.%u.%u", &se, &sh, &cu) == 2 && se == 4 && sh == 5);
	assert(sscanf(" -42 7", "%ld", &value) == 1 && value == -42);
	assert(sscanf("BF16,FP8", "%7[^,],%7s", first, second) == 2);
	assert(!strcmp(first, "BF16") && !strcmp(second, "FP8"));
	/* Bases, widths, %n, %c and %%. */
	unsigned int hex;
	assert(sscanf("0x1fz", "%x%n", &hex, &consumed) == 1 && hex == 0x1f && consumed == 4);
	assert(sscanf("0755", "%i", &number) == 1 && number == 0755);
	assert(sscanf("0x7f", "%i", &number) == 1 && number == 0x7f);
	assert(sscanf("12345", "%3d%d", &number, &consumed) == 2 && number == 123 && consumed == 45);
	assert(sscanf("ab", "%2c", first) == 1 && first[0] == 'a' && first[1] == 'b');
	assert(sscanf("100%", "%d%%", &number) == 1 && number == 100);
	unsigned char byte;
	unsigned long long big;
	assert(sscanf("255 18446744073709551615", "%hhu %llu", &byte, &big) == 2);
	assert(byte == 255 && big == ULLONG_MAX);
	/* A mismatch stops the scan; a '[' without a width is refused. */
	assert(sscanf("x1", "%d", &number) == 0);
	assert(sscanf("abc", "%[abc]", first) == 0);
	assert(sscanf("abc", "%2[abc]", first) == 1 && !strcmp(first, "ab"));
	printf("Linux formatted input: conversions, widths, sets and amdgpu's formats passed\n");
	return 0;
}
#endif

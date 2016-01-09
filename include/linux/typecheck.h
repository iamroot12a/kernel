#ifndef TYPECHECK_H_INCLUDED
#define TYPECHECK_H_INCLUDED

/*
 * Check at compile time that something is of a particular type.
 * Always evaluates to 1 so you may use it easily in comparisons.
 */

/* IAMROOT-12A:
 * ------------
 * typecheck(unsigned long, abc)로 진입한 경우
 * 아래 코드는 다음과 같다.
 *    unsigned long __dummy;
 *    typeof(abc) __dummy2;
 *    (void) (&__dummy == &__dummy2);
 *    1;
 *
 *  위의 코드와 같이 항상 false로 동작할 dummy의 주소값을 비교하는 이유는
 *  데이터 타입이 다른 경우 컴파일 시 warnning이 나도도록 하는데 목적이 있다.
 *
 *  1;을 사용하는 목적은?
 *      추정: 1을 리턴한다.
 */

#define typecheck(type,x) \
({	type __dummy; \
	typeof(x) __dummy2; \
	(void)(&__dummy == &__dummy2); \
	1; \
})

/*
 * Check at compile time that 'function' is a certain type, or is a pointer
 * to that type (needs to use typedef for the function type.)
 */
#define typecheck_fn(type,function) \
({	typeof(type) __tmp = function; \
	(void)__tmp; \
})

#endif		/* TYPECHECK_H_INCLUDED */

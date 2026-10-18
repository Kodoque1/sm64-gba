/* Host reference: SSE sqrtss is a correctly rounded IEEE single-precision
 * square root, the same result the N64's sqrt.s produces. */
float sqrtf(float x) {
    return __builtin_sqrtf(x);
}

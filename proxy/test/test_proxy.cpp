// Unit test for the DFEngine proxy, run under host wine.
// Usage: test_proxy.exe <path to DFEngine.dll>
// With no DFEngine_orig.dll beside this exe, the proxy must return the stub,
// whose Start fails and whose other slots return with a balanced stack.
#include <windows.h>
#include <stdio.h>

typedef void *(__cdecl *GetDFEngine_t)(void);
typedef int(__stdcall *Start_t)(void *, unsigned, void *);
typedef int(__stdcall *One_t)(void *);
typedef int(__stdcall *Two_t)(void *, void *);
typedef int(__stdcall *Create_t)(void *, int, int, void *, void **);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_proxy dll\n"); return 2; }
    HMODULE m = LoadLibraryA(argv[1]);
    CHECK(m != NULL);
    if (!m) return 1;
    GetDFEngine_t g = (GetDFEngine_t)(void *)GetProcAddress(m, "GetDFEngine");
    CHECK(g != NULL);
    if (!g) return 1;
    void *e = g();
    CHECK(e != NULL);
    CHECK(g() == e); // stable on repeat calls
    void **vt = *(void ***)e;
    int cfg[16] = {0};
    CHECK(((Start_t)vt[1])(e, 0x4050000, cfg) != 0);
    void *out = (void *)1;
    CHECK(((Create_t)vt[4])(e, 1, 2, cfg, &out) != 0 && out == NULL);
    CHECK(((One_t)vt[2])(e) == 0);
    CHECK(((One_t)vt[8])(e) == 0);
    int one_arg_slots[] = {5, 6, 7, 9, 10, 14, 15, 18};
    for (int s : one_arg_slots)
        CHECK(((Two_t)vt[s])(e, cfg) == 0);
    printf("%s (%d failures)\n", fails ? "TEST FAILED" : "TEST OK", fails);
    return fails ? 1 : 0;
}

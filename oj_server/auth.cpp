#include "auth.h"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <cstdio>
#include <sstream>

using namespace std;

vector<string> split(const string &s, char delim)
{
    vector<string> out;
    istringstream iss(s);
    string piece;
    while (getline(iss, piece, delim))
    {
        out.push_back(piece);
    }
    return out;
}

bool to_long(const string &s, long &out)
{
    size_t pos = 0;
    try
    {
        out = stol(s, &pos);
    }
    catch (...)
    {
        return false;
    }
    return pos == s.size();
}

bool parse_scrypt(const string &stored,
                  long &N, long &r, long &p,
                  string &salt, string &expected_hex)
{
    vector<string> parts = split(stored, '$');
    if (parts.size() != 3)
    {
        return false;
    }
    vector<string> seg = split(parts[0], ':');
    if (seg.size() != 4 || seg[0] != "scrypt")
    {
        return false;
    }

    salt = parts[1];
    expected_hex = parts[2];

    if (salt.empty() || expected_hex.empty())
    {
        return false;
    }

    if (expected_hex.size() % 2 != 0)
    {
        return false;
    }

    if (expected_hex.find_first_not_of("0123456789abcdef") != string::npos)
    {
        return false;
    }

    if (!to_long(seg[1], N) || !to_long(seg[2], r) || !to_long(seg[3], p))
    {
        return false;
    }

    return true;
}

bool scrypt_verify(const string &password, const string &stored)
{
    long N, r, p;
    string salt, expected_hex;
    if (!parse_scrypt(stored, N, r, p, salt, expected_hex))
    {
        return false;
    }

    vector<unsigned char> out(expected_hex.size() / 2);

    int rc = EVP_PBE_scrypt(password.data(), password.size(),
                            (const unsigned char *)salt.data(), salt.size(),
                            N, r, p,
                            132L * N * r * p,   
                            out.data(), out.size());
    if (rc != 1)
    {
        return false;
    }

    string got;
    got.reserve(out.size() * 2);
    for (unsigned char c : out)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", c);
        got += buf;
    }

    return CRYPTO_memcmp(got.data(), expected_hex.data(), got.size()) == 0;
}
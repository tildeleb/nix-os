#include "std.h"
#include "dat.h"

/*
 * RSA authentication.
 * 
 * Encrypt/Decrypt:
 *	start n=xxx ek=xxx
 *	write msg
 *	read encrypt/decrypt(msg)
 *
 * Sign (PKCS #1 using hash=sha1 or hash=md5)
 *	start n=xxx ek=xxx
 *	write hash(msg)
 *	read signature(hash(msg))
 * 
 * Verify:
 *	start n=xxx ek=xxx
 *	write hash(msg)
 *	write signature(hash(msg))
 *	read ok or fail
 *
 * all numbers are hexadecimal biginits parsable with strtomp.
 * must be lower case for attribute matching in start.
 */

static int
convwritemp(Conv *c, mpint *m)
{
	char *s;
	int n;

	s = smprint("%B", m);
	n = convwrite(c, s, strlen(s));
	free(s);
	return n;
}

static int
xrsaclient(Conv *c)
{
	char *s;
	int n, ret;
	mpint *m;
	Key *k;
	RSApriv *key;

	flog("rsa client: warning depricated ssh1 client");
	ret = -1;
	m = nil;

	/* fetch key */
	c->state = "keylookup";
	k = keylookup("%A", c->attr);
	if(k == nil)
		goto out;
	key = k->priv;

	/* send response */
	c->state = "write pub";
	convwritemp(c, key->pub.n);

	c->state = "read chal";
	n = convreadm(c, &s);
	s[n] = 0;
	m = strtomp(s, nil, 16, nil);
	if(m == nil){
		werrstr("invalid challenge value");
		goto out;
	}
	m = rsadecrypt(key, m, m);

	c->state = "established";
	convwritemp(c, m);
	ret = 0;

out:
	keyclose(k);
	mpfree(m);
	return ret;
}

static int
xrsadecrypt(Conv *c)
{
	char *txt, *be, *role;
	int n, ret;
	mpint *m, *mm;
	Key *k;
	RSApriv *key;

	ret = -1;
	txt = nil;
	m = nil;
	mm = nil;
	be = nil;

	/* fetch key */
	c->state = "keylookup";
	k = keylookup("%A", c->attr);
	if(k == nil)
		goto out;
	key = k->priv;
	
	/* make sure have private half if needed */
	role = strfindattr(c->attr, "role");
	if(strcmp(role, "decrypt") == 0 && !key->c2){
		werrstr("missing private half of key -- cannot decrypt");
		goto out;
	}
	
	/* read text */
	c->state = "read";
	if((n=convreadm(c, &txt)) < 0)
		goto out;
	if(n < 32){
		convprint(c, "data too short");
		goto out;
	}

	/* encrypt/decrypt */
	m = betomp((uchar*)txt, n, nil);
	if(m == nil)
		goto out;
	if(strcmp(role, "decrypt") == 0)
		mm = rsadecrypt(key, m, nil);
	else
		mm = rsaencrypt(&key->pub, m, nil);
	if(mm == nil)
		goto out;
	be = malloc(4096);
	n = mptobe(mm, (uchar*)be, 4096, nil);
	
	/* send response */
	c->state = "write";
	convwrite(c, be, n);
	ret = 0;

out:
	free(be);
	mpfree(m);
	mpfree(mm);
	keyclose(k);
	free(txt);
	return ret;
}

static int
xrsasign(Conv *c)
{
	char *hash, *role;
	int dlen, n, ret;
	DigestAlg *hashfn;
	Key *k;
	RSApriv *key;
	uchar sig[1024], digest[64];
	char *sig2;

	ret = -1;

	/* fetch key */
	c->state = "keylookup";
	k = keylookup("%A", c->attr);
	if(k == nil)
		goto out;

	/* make sure have private half if needed */
	key = k->priv;
	role = strfindattr(c->attr, "role");
	if(strcmp(role, "sign") == 0 && !key->c2){
		werrstr("missing private half of key -- cannot sign");
		goto out;
	}
	
	/* get hash type from key */
	hash = strfindattr(k->attr, "hash");
	if(hash == nil)
		hash = "sha1";
	if(strcmp(hash, "sha1") == 0){
		hashfn = sha1;
		dlen = SHA1dlen;
	}else if(strcmp(hash, "md5") == 0){
		hashfn = md5;
		dlen = MD5dlen;
	}else{
		werrstr("unknown hash function %s", hash);
		goto out;
	}

	/* read hash */
	c->state = "read hash";
	if(convread(c, digest, dlen) < 0)
		goto out;

	if(strcmp(role, "sign") == 0){
		/* sign */
		if((n=rsasign(key, hashfn, digest, dlen, sig, sizeof sig)) < 0)
			goto out;

		/* write */
		convwrite(c, sig, n);
	}else{
		/* read signature */
		if((n = convreadm(c, &sig2)) < 0)
			goto out;

		/* verify */
		if(rsaverify(&key->pub, hashfn, digest, dlen, (uchar*)sig2, n) == 0)
			convprint(c, "ok");
		else
			convprint(c, "signature does not verify");
		free(sig2);
	}
	ret = 0;

out:
	keyclose(k);
	return ret;
}

/*
 * convert to canonical form (lower case) 
 * for use in attribute matches.
 */
static void
strlwr(char *a)
{
	for(; *a; a++){
		if('A' <= *a && *a <= 'Z')
			*a += 'a' - 'A';
	}
}

static RSApriv*
readrsapriv(Key *k)
{
	char *a;
	RSApriv *priv;

	priv = rsaprivalloc();

	if((a=strfindattr(k->attr, "ek"))==nil 
	|| (priv->pub.ek=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->attr, "n"))==nil 
	|| (priv->pub.n=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if(k->privattr == nil)	/* only public half */
		return priv;

	if((a=strfindattr(k->privattr, "!p"))==nil 
	|| (priv->p=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->privattr, "!q"))==nil 
	|| (priv->q=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->privattr, "!kp"))==nil 
	|| (priv->kp=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->privattr, "!kq"))==nil 
	|| (priv->kq=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->privattr, "!c2"))==nil 
	|| (priv->c2=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	if((a=strfindattr(k->privattr, "!dk"))==nil 
	|| (priv->dk=strtomp(a, nil, 16, nil))==nil)
		goto Error;
	strlwr(a);
	return priv;

Error:
	rsaprivfree(priv);
	return nil;
}

static int
rsacheck(Key *k)
{
	static int first = 1;
	
	if(first){
		fmtinstall('B', mpfmt);
		first = 0;
	}

	if((k->priv = readrsapriv(k)) == nil){
		werrstr("malformed key data");
		return -1;
	}
	return 0;
}

static void
rsaclose(Key *k)
{
	rsaprivfree(k->priv);
	k->priv = nil;
}

static Role
rsaroles[] = 
{
	"client",	xrsaclient, 		/* old crap.  support old ssh */
	"sign",	xrsasign,
	"verify",	xrsasign,	/* public operation */
	"decrypt",	xrsadecrypt,
	"encrypt",	xrsadecrypt,	/* public operation */
	0
};

Proto rsa = {
	"rsa",
	rsaroles,
	nil,
	rsacheck,
	rsaclose
};

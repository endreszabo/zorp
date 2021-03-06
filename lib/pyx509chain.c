/***************************************************************************
 *
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009,
 * 2010, 2011 BalaBit IT Ltd, Budapest, Hungary
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Note that this permission is granted for only version 2 of the GPL.
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Author: Laszlo Attila Toth
 *
 ***************************************************************************/

#include <zorp/pyx509.h>
#include <zorp/zpython.h>
#include <zorp/certchain.h>

#include <zorp/log.h>

#include <openssl/pem.h>

#define PROXY_SSL_EXTRACT_PEM(s, l, r) \
  ({ void *p; BIO *bio = BIO_new_mem_buf(s, l); p = r(bio, NULL, NULL, NULL); BIO_free(bio); p; })

typedef struct _ZorpCertificateChain
{
  PyObject_HEAD
  ZCertificateChain *chain;
} ZorpCertificateChain;

static PyTypeObject z_py_zorp_certificate_chain_type;

static PyObject *
z_py_zorp_certificate_chain_new(ZCertificateChain *chain)
{
  ZorpCertificateChain *self;

  if (chain)
    {
      self = PyObject_New(ZorpCertificateChain, &z_py_zorp_certificate_chain_type);
      self->chain = chain;
      z_object_ref(&self->chain->super);
      return (PyObject *) self;
    }
  else
    {
      return z_policy_none_ref();
    }
}

static PyObject *
z_py_zorp_certificate_chain_convert_x509_to_pystring(X509 *cert)
{
  PyObject *res = NULL;
  gchar *mem;
  guint len;
  BIO *bio = BIO_new(BIO_s_mem());

  PEM_write_bio_X509(bio, cert);
  len = BIO_get_mem_data(bio, &mem);
  res = PyString_FromStringAndSize(mem, len);

  BIO_free(bio);

  return res;
}

static PyObject *
z_py_zorp_certificate_chain_getattr(PyObject *o, char *name)
{
  ZorpCertificateChain *self = (ZorpCertificateChain *) o;
  PyObject *res = NULL;
  gchar buf[512];

  if (strcmp(name, "blob") == 0)
    {
      return z_py_zorp_certificate_chain_convert_x509_to_pystring(z_certificate_chain_get_cert(self->chain));
    }
  else if (strcmp(name, "chain") == 0)
  {
    /* recursively --> list of strings */
    gsize chain_len = z_certificate_chain_get_chain_length(self->chain);
    res = PyList_New(chain_len);

    for (gsize i = 0; i != chain_len; ++i)
      PyList_SET_ITEM(res, i, z_py_zorp_certificate_chain_convert_x509_to_pystring(z_certificate_chain_get_cert_from_chain(self->chain, i)));

    return res;
  }
  else if (strcmp(name, "issuer") == 0)
    {
      X509_NAME_oneline(X509_get_issuer_name(z_certificate_chain_get_cert(self->chain)), buf, sizeof(buf));
      res = PyString_FromString(buf);
    }
  else if (strcmp(name, "subject") == 0)
    {
      X509_NAME_oneline(X509_get_subject_name(z_certificate_chain_get_cert(self->chain)), buf, sizeof(buf));
      res = PyString_FromString(buf);
    }
  else if (strcmp(name, "serial") == 0)
    {
      ASN1_INTEGER *cert_serial;

      cert_serial = X509_get_serialNumber(z_certificate_chain_get_cert(self->chain));

      if (cert_serial)
        {
          res = PyInt_FromLong(ASN1_INTEGER_get(cert_serial));
        }
    }
  else
    {
      PyErr_SetString(PyExc_AttributeError, "Attribute not found");
    }
  return res;
}

static void
z_py_zorp_certificate_chain_free(ZorpCertificateChain *self)
{
  z_object_unref(&self->chain->super);
  PyObject_Del(self);
}

static PyTypeObject z_py_zorp_certificate_chain_type =
{
  PyObject_HEAD_INIT(&PyType_Type)
  .tp_name = "Zorp Certificate Chain",
  .tp_basicsize = sizeof(ZorpCertificateChain),
  .tp_dealloc = (destructor) z_py_zorp_certificate_chain_free,
  .tp_getattr = z_py_zorp_certificate_chain_getattr,
  .tp_doc = "ZorpCertificateChain class for Zorp",
};

ZPolicyObj *
z_py_ssl_certificate_chain_get(ZProxy *self G_GNUC_UNUSED, gchar *name G_GNUC_UNUSED, gpointer value)
{
  ZCertificateChain **chain = (ZCertificateChain **) value;

  return z_py_zorp_certificate_chain_new(*chain);
}

/**
 * Adds certificates to the chain, iterating thru the specified input string.
 *
 * @param chain The certificate and its chain
 * @param input The string that may contain additional certificates which will be added to the chain
 * @param input_len The length of the input string
 * @return TRUE on success, FALSE on error
 */
static gboolean
z_py_ssl_certificate_chain_set_chain(ZCertificateChain *chain, gchar *input, gsize input_len)
{
  char *next = g_strstr_len(input, input_len, "-----BEGIN CERTIFICATE-----");

  if (next)
    {
      gssize next_len = input_len - (next - input);

      X509 *cert = PROXY_SSL_EXTRACT_PEM(next, next_len, PEM_read_bio_X509);

      if (!cert)
      {
        PyErr_SetString(PyExc_TypeError, "Certificate chains must be specified as strings in PEM format.");
        return FALSE;
      }

      z_certificate_chain_add_cert_to_chain(chain, cert);
      X509_free(cert);

      if (next_len > 1)
        return z_py_ssl_certificate_chain_set_chain(chain, next + 1, next_len - 1);
    }

    return TRUE;
}

int
z_py_ssl_certificate_chain_set(ZProxy *self G_GNUC_UNUSED, gchar *name G_GNUC_UNUSED, gpointer value, ZPolicyObj *new)
{
  ZCertificateChain **chain = (ZCertificateChain **) value;

  if (*chain)
    {
      z_object_unref(&(*chain)->super);
      *chain = NULL;
    }

  if (PyString_Check(new))
    {
      gchar *input = PyString_AsString(new);
      gsize  input_len = PyString_Size(new);

      char *next = g_strstr_len(input, input_len, "-----BEGIN CERTIFICATE-----");

      if (!next)
        goto err_msg;
      else
        {
          input_len -= (next - input);
          input = next;
        }

      if (input_len)
        {
          X509 *cert = PROXY_SSL_EXTRACT_PEM(input, input_len, PEM_read_bio_X509);

          if (!cert)
            goto err_msg;

          *chain = z_certificate_chain_new();
          z_certificate_chain_set_cert(*chain, cert);
          X509_free(cert);

          if (!z_py_ssl_certificate_chain_set_chain(*chain, input + 1, input_len - 1))
            goto err_out;
        }
    }

  return 0;

  err_msg:
    PyErr_SetString(PyExc_TypeError, "Certificate chains must be specified as strings in PEM format.");
  err_out:
    return -1;
}

void
z_py_ssl_certificate_chain_free(gpointer value)
{
  ZCertificateChain **chain = (ZCertificateChain **) value;

  if (*chain)
    z_object_unref(&(*chain)->super);
}

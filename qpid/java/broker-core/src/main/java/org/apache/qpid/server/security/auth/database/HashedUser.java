/*
*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
package org.apache.qpid.server.security.auth.database;

import org.apache.commons.codec.binary.Base64;
import org.apache.log4j.Logger;
import org.apache.qpid.server.util.ServerScopedRuntimeException;

import java.io.UnsupportedEncodingException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;


public class HashedUser implements PasswordPrincipal
{
    private static final Logger _logger = Logger.getLogger(HashedUser.class);

    private String _name;
    private char[] _password;
    private byte[] _encodedPassword = null;
    private boolean _modified = false;
    private boolean _deleted = false;

    HashedUser(String[] data)
    {
        if (data.length != 2)
        {
            throw new IllegalArgumentException("User Data should be length 2, username, password");
        }

        _name = data[0];

        byte[] encoded_password;
        try
        {
            encoded_password = data[1].getBytes(Base64MD5PasswordFilePrincipalDatabase.DEFAULT_ENCODING);
        }
        catch (UnsupportedEncodingException e)
        {
            throw new ServerScopedRuntimeException("MD5 encoding not supported, even though the Java standard requires it",e);
        }

        Base64 b64 = new Base64();
        byte[] decoded = b64.decode(encoded_password);

        _encodedPassword = encoded_password;

        _password = new char[decoded.length];

        int index = 0;
        for (byte c : decoded)
        {
            _password[index++] = (char) c;
        }
    }

    public HashedUser(String name, char[] password)
    {
        _name = name;
        setPassword(password,false);
    }

    public static byte[] getMD5(byte[] data)
    {
        MessageDigest md = null;
        try
        {
            md = MessageDigest.getInstance("MD5");
        }
        catch (NoSuchAlgorithmException e)
        {
            throw new ServerScopedRuntimeException("MD5 not supported although Java compliance requires it");
        }

        for (byte b : data)
        {
            md.update(b);
        }

        return md.digest();
    }

    public String getName()
    {
        return _name;
    }

    public String toString()
    {
        return _name;
    }

    public char[] getPassword()
    {
        return _password;
    }

    public void setPassword(char[] password)
    {
        setPassword(password, false);
    }

    public void restorePassword(char[] password)
    {
        setPassword(password, true);
    }

    void setPassword(char[] password, boolean alreadyHashed)
    {
        if(alreadyHashed){
            _password = password;
        }
        else
        {
            byte[] byteArray = new byte[password.length];
            int index = 0;
            for (char c : password)
            {
                byteArray[index++] = (byte) c;
            }
            
            byte[] MD5byteArray = getMD5(byteArray);
            
            _password = new char[MD5byteArray.length];

            index = 0;
            for (byte c : MD5byteArray)
            {
                _password[index++] = (char) c;
            }
        }
        
        _modified = true;
        _encodedPassword = null;
    }

    public byte[] getEncodedPassword()
    {
        if (_encodedPassword == null)
        {
            encodePassword();
        }
        return _encodedPassword;
    }

    private void encodePassword()
    {
        byte[] byteArray = new byte[_password.length];
        int index = 0;
        for (char c : _password)
        {
            byteArray[index++] = (byte) c;
        }
        _encodedPassword = (new Base64()).encode(byteArray);
    }

    public boolean isModified()
    {
        return _modified;
    }

    public boolean isDeleted()
    {
        return _deleted;
    }

    public void delete()
    {
        _deleted = true;
    }

    public void saved()
    {
        _modified = false;
    }

}

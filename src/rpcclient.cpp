// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <set>
#include <rpcclient.h>

#include <rpcprotocol.h>
#include <util.h>
#include <ui_interface.h>
#include <chainparams.h> // for Params().RPCPort()

#include <stdint.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>

#include <json/json_spirit_writer_template.h>


json_spirit::Object CallRPC(const std::string& strMethod, const json_spirit::Array& params)
{

    // Connect to localhost
    bool fUseSSL = GetBoolArg("-rpcssl", false);
    boost::asio::io_service io_service;
    boost::asio::ssl::context context(boost::asio::ssl::context::sslv23);
    context.set_options(boost::asio::ssl::context::no_sslv2);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> sslStream(io_service, context);
    SSLIOStreamDevice<boost::asio::ip::tcp> d(sslStream, fUseSSL);
    boost::iostreams::stream< SSLIOStreamDevice<boost::asio::ip::tcp> > stream(d);

    bool fWait = GetBoolArg("-rpcwait", false); // -rpcwait means try until server has started
    do {
        bool fConnected = d.connect(GetArg("-rpcconnect", "127.0.0.1"), GetArg("-rpcport", itostr(Params().RPCPort())));
        if (fConnected) break;
        if (fWait)
            MilliSleep(1000);
        else
            throw std::runtime_error("couldn't connect to server");
    } while (fWait);

    // Find credentials to use
     std::string strRPCUserColonPass;
     if (mapArgs["-rpcpassword"] == "") {
         // Try fall back to cookie-based authentication if no password is provided
         if (!GetAuthCookie(&strRPCUserColonPass)) {
             throw std::runtime_error(strprintf(
                 _("You must set rpcpassword=<password> in the configuration file:\n%s\n"
                   "If the file does not exist, create it with owner-readable-only file permissions."),
                     GetConfigFile().string().c_str()));

         }
     } else {
         strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
     }

    // HTTP basic authentication
    std::map<std::string, std::string> mapRequestHeaders;
    mapRequestHeaders["Authorization"] = std::string("Basic ") + EncodeBase64(strRPCUserColonPass);

    // Send request
    std::string strRequest = JSONRPCRequest(strMethod, params, 1);
    std::string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive HTTP reply status
    int nProto = 0;
    int nStatus = ReadHTTPStatus(stream, nProto);

    // Receive HTTP reply message headers and body
    std::map<std::string, std::string> mapHeaders;
    std::string strReply;
    ReadHTTPMessage(stream, mapHeaders, strReply, nProto);

    if (nStatus == HTTP_UNAUTHORIZED)
        throw std::runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw std::runtime_error(strprintf("server returned HTTP error %d", nStatus));
    else if (strReply.empty())
        throw std::runtime_error("no response from server");

    // Parse reply
    json_spirit::Value valReply;
    if (!json_spirit::read_string(strReply, valReply))
        throw std::runtime_error("couldn't parse reply from server");
    const json_spirit::Object& reply = valReply.get_obj();
    if (reply.empty())
        throw std::runtime_error("expected reply to have result, error and id properties");

    return reply;
}


class CRPCConvertParam
{
public:
    std::string methodName;            // method whose params want conversion
    int paramIdx;                      // 0-based idx of param to convert
};

static const CRPCConvertParam vRPCConvertParams[] =
{
    { "stop", 0 },
    { "getaddednodeinfo", 0 },
    { "sendtoaddress", 1 },
    { "settxfee", 0 },
    { "getreceivedbyaddress", 1 },
    { "getreceivedbyaccount", 1 },
    { "listreceivedbyaddress", 0 },
    { "listreceivedbyaddress", 1 },
    { "listreceivedbyaccount", 0 },
    { "listreceivedbyaccount", 1 },
    { "getbalance", 1 },
    { "getblock", 1 },
    { "getblockbynumber", 0 },
    { "getblockbynumber", 1 },
    { "getblockhash", 0 },
    { "move", 2 },
    { "move", 3 },
    { "sendfrom", 2 },
    { "sendfrom", 3 },
    { "listtransactions", 1 },
    { "listtransactions", 2 },
    { "listaccounts", 0 },
    { "walletpassphrase", 1 },
    { "walletpassphrase", 2 },
    { "getblocktemplate", 0 },
    { "listsinceblock", 1 },
    { "sendmany", 1 },
    { "sendmany", 2 },
    { "reservebalance", 0 },
    { "reservebalance", 1 },
    { "addmultisigaddress", 0 },
    { "addmultisigaddress", 1 },
    { "listunspent", 0 },
    { "listunspent", 1 },
    { "listunspent", 2 },
    { "getrawtransaction", 1 },
    { "createrawtransaction", 0 },
    { "createrawtransaction", 1 },
    { "signrawtransaction", 1 },
    { "signrawtransaction", 2 },
    { "keypoolrefill", 0 },
    { "importprivkey", 2 },
    { "checkkernel", 0 },
    { "checkkernel", 1 },
};

class CRPCConvertTable
{
private:
    std::set<std::pair<std::string, int> > members;

public:
    CRPCConvertTable();

    bool convert(const std::string& method, int idx) {
        return (members.count(std::make_pair(method, idx)) > 0);
    }
};

CRPCConvertTable::CRPCConvertTable()
{
    const unsigned int n_elem =
        (sizeof(vRPCConvertParams) / sizeof(vRPCConvertParams[0]));

    for (unsigned int i = 0; i < n_elem; i++) {
        members.insert(std::make_pair(vRPCConvertParams[i].methodName,
                                      vRPCConvertParams[i].paramIdx));
    }
}

static CRPCConvertTable rpcCvtTable;

// Convert strings to command-specific RPC representation
json_spirit::Array RPCConvertValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    json_spirit::Array params;

    for (unsigned int idx = 0; idx < strParams.size(); idx++) {
        const std::string& strVal = strParams[idx];

        // insert string value directly
        if (!rpcCvtTable.convert(strMethod, idx)) {
            params.push_back(strVal);
        }

        // parse string as JSON, insert bool/number/object/etc. value
        else {
            json_spirit::Value jVal;
            if (!json_spirit::read_string(strVal, jVal))
                throw std::runtime_error(std::string("Error parsing JSON:")+strVal);
            params.push_back(jVal);
        }

    }

    return params;
}

int CommandLineRPC(int argc, char *argv[])
{
    std::string strPrint;
    int nRet = 0;
    try
    {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0]))
        {
            argc--;
            argv++;
        }

        // Method
        if (argc < 2)
            throw std::runtime_error("too few parameters");
        std::string strMethod = argv[1];

        // Parameters default to strings
        std::vector<std::string> strParams(&argv[2], &argv[argc]);
        json_spirit::Array params = RPCConvertValues(strMethod, strParams);

        // Execute
        json_spirit::Object reply = CallRPC(strMethod, params);

        // Parse reply
        const json_spirit::Value& result = json_spirit::find_value(reply, "result");
        const json_spirit::Value& error  = json_spirit::find_value(reply, "error");

        if (error.type() != json_spirit::null_type)
        {
            // Error
            strPrint = "error: " + json_spirit::write_string(error, false);
            int code = json_spirit::find_value(error.get_obj(), "code").get_int();
            nRet = abs(code);
        }
        else
        {
            // Result
            if (result.type() == json_spirit::null_type)
                strPrint = "";
            else if (result.type() == json_spirit::str_type)
                strPrint = result.get_str();
            else
                strPrint = json_spirit::write_string(result, true);
        }
    }
    catch (boost::thread_interrupted) {
        throw;
    }
    catch (std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = 87;
    }
    catch (...) {
        PrintException(nullptr, "CommandLineRPC()");
    }

    if (strPrint != "")
    {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }
    return nRet;
}

/***************************************************************************
* Copyright (c) 2016, Johan Mabille, Loic Gouarin and Sylvain Corlay       *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/
#ifndef XINSPECT_HPP
#define XINSPECT_HPP

#include <cxxabi.h>
#include <fstream>
#include <pugixml.hpp>
#include <string>

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Value.h"
#include "cling/Utils/Output.h"

#include "xparser.hpp"

namespace xeus
{
    struct node_predicate
    {
        std::string kind;
        std::string child_value;

        bool operator()(pugi::xml_node node) const
        {
            return static_cast<std::string>(node.attribute("kind").value()) == kind && static_cast<std::string>(node.child("name").child_value()) == child_value;
        }
    };

    struct class_member_predicate
    {
        std::string class_name;
        std::string kind;
        std::string child_value;
        std::string member_file;

        std::string get_filename(pugi::xml_node node)
        {
            for (pugi::xml_node child : node.children())
            {
                if (static_cast<std::string>(child.attribute("kind").value()) == kind && static_cast<std::string>(child.child("name").child_value()) == child_value)
                    return child.child("anchorfile").child_value();
            }
        }

        bool operator()(pugi::xml_node node)
        {
            auto parent = (static_cast<std::string>(node.attribute("kind").value()) == "class" || static_cast<std::string>(node.attribute("kind").value()) == "struct") && static_cast<std::string>(node.child("name").child_value()) == class_name;
            auto found = false;
            if (parent)
                for (pugi::xml_node child : node.children())
                {
                    if (static_cast<std::string>(child.attribute("kind").value()) == kind && static_cast<std::string>(child.child("name").child_value()) == child_value)
                    {
                        found = true;
                        break;
                    }
                }
            return found;
        }
    };

    std::string find_type(const std::string& expression, cling::MetaProcessor& m_processor)
    {
        cling::Interpreter::CompilationResult compilation_result;
        cling::Value result;
        std::string typeString;

        // add typeinfo in include files in order to use typeid
        std::string code = "#include <typeinfo>";
        m_processor.process(code.c_str(), compilation_result, &result);

        // try to find the typename of the class
        code = "typeid("+ expression + ").name();";

        // Temporarily dismissing all std::cerr and std::cout resulting from `m_processor.process`
        auto errorlevel = 0;
        {
            auto cout_strbuf = std::cout.rdbuf();
            auto cerr_strbuf = std::cerr.rdbuf();
            auto null = xnull();
            std::cout.rdbuf(&null);
            std::cerr.rdbuf(&null);

            errorlevel = m_processor.process(code.c_str(), compilation_result, &result);

            std::cout.rdbuf(cout_strbuf);
            std::cerr.rdbuf(cerr_strbuf);
        }

        if (errorlevel)
        {
            m_processor.cancelContinuation();
        }
        else if (compilation_result == cling::Interpreter::kSuccess)
        {
            // we found the typeid
            std::string valueString;
            {
                llvm::raw_string_ostream os(valueString);
                result.print(os);
            }

            // search the typename in the output between ""
            std::regex re_typename("\\\"(.*)\\\"");
            std::smatch typename_;
            std::regex_search(valueString, typename_, re_typename);
            int status;
            // set in valueString the typename given by typeid
            valueString = typename_.str(1);
            // we need demangling in order to have its string representation
            valueString = abi::__cxa_demangle(valueString.c_str(), 0, 0, &status);
            
            re_typename = "(\\w*(?:\\:{2}?\\w*)*)";
            std::regex_search(valueString, typename_, re_typename);
            if (!typename_.str(1).empty())
            {
                typeString = typename_[1];
            }
        }

        return typeString;
    }

    std::string inspect(const std::string& code, cling::MetaProcessor& m_processor)
    {

        std::string tagfile_dir = TAGFILE_DIR;

        std::vector<std::string> check{"class", "struct", "function"};

        std::string search_file = tagfile_dir + "/search_list.txt";
        std::ifstream search(search_file);

        std::string url, tagfile;

        // method or variable of class found found (xxxx.yyyy)
        if (std::regex_search(code, std::regex{"\\.\\w*\\?\\?"}))
        {
            std::regex re_expression("((((?:\\w*(?:(?:\\:{2})|(?:\\<(?:.*)\\>)|(?:\\(.*\\))|(?:\\[.*\\]))?))\\.?)*)\\?\\?");
            std::smatch expression;
            std::regex_search(code, expression, re_expression);
            std::regex re_method("(.*)\\.(\\w*)");
            std::smatch method;
            std::string tmp = expression[1];

            // method[1]: xxxx method[2]: yyyy
            std::regex_search(tmp, method, re_method);
            std::string typename_ = find_type(method[1], m_processor);

            if (!typename_.empty())
            {
                while(search >> url >> tagfile)
                {
                    std::string filename = tagfile_dir + "/" + tagfile;
                    pugi::xml_document doc;
                    pugi::xml_parse_result result = doc.load_file(filename.c_str());
                    class_member_predicate predicate{typename_, "function", method[2]};
                    auto node = doc.find_node(predicate);
                    if (!node.empty())
                    {
                        return url + predicate.get_filename(node);
                    }
                }
            }
        }
        else
        {
            std::regex re_expression("((((?:\\w*(?:(?:\\:{2})|(?:\\<(?:.*)\\>)|(?:\\(.*\\))|(?:\\[.*\\]))?)))*)\\?\\?");
            std::smatch to_inspect;
            std::regex_search(code, to_inspect, re_expression);

            std::string typename_ = find_type(to_inspect[1], m_processor);
            std::string findString = (typename_.empty())? to_inspect[1]: typename_;
             
            while(search >> url >> tagfile)
            {
                std::string filename = tagfile_dir + "/" + tagfile;
                pugi::xml_document doc;
                pugi::xml_parse_result result = doc.load_file(filename.c_str());
                for (auto c : check)
                {
                    node_predicate predicate{c, findString};
                    std::string node;

                    if (c == "class" || c == "struct")
                    {
                        node = doc.find_node(predicate).child("filename").child_value();
                    }
                    else
                    {
                        node = doc.find_node(predicate).child("anchorfile").child_value();
                    }

                    if (!node.empty())
                    {
                       return url + node;
                    }
                }
            }
        }
        return "";
    }
}
#endif

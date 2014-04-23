//
//  container.cpp
//  ePub3
//
//  Created by Jim Dovey on 2012-11-28.
//  Copyright (c) 2012-2013 The Readium Foundation and contributors.
//  
//  The Readium SDK is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//  
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//  
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "package.h"
#include "container.h"
#include "archive.h"
#include "archive_xml.h"
#include "xpath_wrangler.h"
#include "byte_stream.h"
#include "filter_manager.h"
#include "ibooks_options.h"
#include <ePub3/xml/document.h>
#include <ePub3/xml/io.h>
#include <ePub3/content_module_manager.h>

EPUB3_BEGIN_NAMESPACE

static const char * gContainerFilePath = "META-INF/container.xml";
static const char * gEncryptionFilePath = "META-INF/encryption.xml";
static const char * gRootfilesXPath = "/ocf:container/ocf:rootfiles/ocf:rootfile";
static const char * gRootfilePathsXPath = "/ocf:container/ocf:rootfiles/ocf:rootfile/@full-path";
static const char * gVersionXPath = "/ocf:container/@version";

template <typename _Tp>
std::future<_Tp> __make_ready_future(_Tp& value)
{
	std::promise<_Tp> __p;
	__p.set_value(value);
	return __p.get_future();
}

Container::Container() :
#if EPUB_PLATFORM(WINRT)
	NativeBridge(),
#endif
	_archive(nullptr), _ocf(nullptr), _packages(), _encryption(), _path()
{
}
Container::Container(Container&& o) :
#if EPUB_PLATFORM(WINRT)
NativeBridge(),
#endif
_archive(std::move(o._archive)), _ocf(o._ocf), _packages(std::move(o._packages)), _path(std::move(o._path))
{
    o._ocf = nullptr;
}
Container::~Container()
{
}
bool Container::Open(const string& path)
{
	_archive = Archive::Open(path.stl_str());
	if (_archive == nullptr)
		throw std::invalid_argument(_Str("Path does not point to a recognised archive file: '", path, "'"));
	_path = path;

	// TODO: Initialize lazily? Doing so would make initialization faster, but require
	// PackageLocations() to become non-const, like Packages().
	ArchiveXmlReader reader(_archive->ReaderAtPath(gContainerFilePath));
	if (!reader) {
		throw std::invalid_argument(_Str("Path does not point to a recognised archive file: '", path, "'"));
	}
#if EPUB_USE(LIBXML2)
	_ocf = reader.xmlReadDocument(gContainerFilePath, nullptr, XML_PARSE_RECOVER|XML_PARSE_NOENT|XML_PARSE_DTDATTR);
#else
	decltype(_ocf) __tmp(reader.ReadDocument(gContainerFilePath, nullptr, /*RESOLVE_EXTERNALS*/ 1));
	_ocf = __tmp;
#endif
	if (!((bool)_ocf))
		return false;

#if EPUB_COMPILER_SUPPORTS(CXX_INITIALIZER_LISTS)
	XPathWrangler xpath(_ocf, { { "ocf", "urn:oasis:names:tc:opendocument:xmlns:container" } });
#else
	XPathWrangler::NamespaceList __ns;
	__ns["ocf"] = OCFNamespaceURI;
	XPathWrangler xpath(_ocf, __ns);
#endif
	xpath.NameDefaultNamespace("ocf");
	xml::NodeSet nodes = xpath.Nodes(gRootfilesXPath);

	if (nodes.empty())
		return false;

	LoadEncryption();

	for (auto n : nodes)
	{
		string type = _getProp(n, "media-type");

		string path = _getProp(n, "full-path");
		if (path.empty())
			continue;

		auto pkg = Package::New(Ptr(), type);
		if (pkg->Open(path))
			_packages.push_back(pkg);
	}

	if (FileExistsAtPath(iBooksOptions::AppleMetadataPath))
	{
		auto basicReader = _archive->ReaderAtPath(iBooksOptions::AppleMetadataPath);
		if (basicReader)
		{
			auto xmlReader = make_unique<ArchiveXmlReader>(std::move(basicReader));

			try
			{
				iBooksOptions opts(std::move(xmlReader));

				if (opts.GetOptionValue("fixed-layout") == "true")
				{
					for (auto pkg : _packages)
					{
						IRI identifier = pkg->MakePropertyIRI("layout", "rendition");
						PropertyPtr prop = pkg->PropertyMatching(identifier);
						
						if (prop == nullptr)
						{
							prop = Property::New(pkg->CastPtr<PropertyHolder>());
							prop->SetPropertyIdentifier(identifier);
							pkg->AddProperty(prop);
						}

						prop->SetValue("pre-paginated");
					}
				}

				const string& opensToSpread = opts.GetOptionValue("open-to-spread");
				if (!opensToSpread.empty())
				{
					for (auto pkg : _packages)
					{
						IRI identifier = pkg->MakePropertyIRI("spread", "rendition");
						PropertyPtr prop = pkg->PropertyMatching(identifier);

						if (prop == nullptr)
						{
							prop = Property::New(pkg->CastPtr<PropertyHolder>());
							prop->SetPropertyIdentifier(identifier);
							pkg->AddProperty(prop);
						}

						if (opensToSpread == "true")
							prop->SetValue("both");
						else
							prop->SetValue("none");
					}
				}

				const string& orientationLock = opts.GetOptionValue("orientation-lock");
				if (!orientationLock.empty())
				{
					for (auto pkg : _packages)
					{
						IRI identifier = pkg->MakePropertyIRI("orientation", "rendition");
						PropertyPtr prop = pkg->PropertyMatching(identifier);

						if (prop == nullptr)
						{
							prop = Property::New(pkg->CastPtr<PropertyHolder>());
							prop->SetPropertyIdentifier(identifier);
							pkg->AddProperty(prop);
						}

						if (orientationLock == "portrait-only")
							prop->SetValue("portrait");
						else if (orientationLock == "landscape-only")
							prop->SetValue("landscape");
						else
							prop->SetValue("auto");
					}
				}

				// TODO: handling for the other known values, possibly
				//  "interactive": boolean, similar to "scripted" manifest item property
				//  "specified-fonts": boolean, unused I believe (we always allow publisher-specified fonts)
			}
#if 0
			catch (std::exception& exc)
			{
				std::cerr << "Exception parsing iBooks Display Options: " << exc.what() << std::endl;
			}
#endif
			catch (...)
			{
			}
		}
	}

	for (auto& pkg : _packages)
	{
		pkg->SetFilterChain(FilterManager::Instance()->BuildFilterChainForPackage(pkg));
	}

	return true;
}
ContainerPtr Container::OpenContainer(const string &path)
{
	auto future = ContentModuleManager::Instance()->LoadContentAtPath(path, launch::any);
	ContainerPtr result;

	// see if it's complete with a nil value
	if (__ar_has_value(future))
	{
		if (future.get().get() == nullptr)
			result = OpenContainerForContentModule(path);
	}

	if (!bool(result))
		result = future.get();

	return result;
}
async_result<ContainerPtr> Container::OpenContainerAsync(const string& path, launch policy)
{
    auto result = ContentModuleManager::Instance()->LoadContentAtPath(path, policy);
    
    // see if it's complete with a nil value
    if (__ar_has_value(result))
    {
		if (result.get().get() == nullptr)
		{
#if EPUB_PLATFORM(WIN_PHONE)
			result = async_result<ContainerPtr>([path]() { return Container::OpenContainerForContentModule(path); });
#else
			result = ::ePub3::async(policy, &Container::OpenContainerForContentModule, path);
#endif
		}
    }
    
    return result;
}
#if EPUB_PLATFORM(WINRT)
ContainerPtr Container::OpenSynchronouslyForWinRT(const string& path)
{
	auto future = ContentModuleManager::Instance()->LoadContentAtPath(path, launch::deferred);

	// see if it's complete with a nil value
	//if (future.wait_for(std::chrono::system_clock::duration(0)) == future_status::ready)
	if (__ar_has_value(future))
	{
		ContainerPtr result = future.get();
		if (bool(result))
			return result;
		else
			return OpenContainerForContentModule(path);
	}

	// deferred call, will run the operation synchronously now
	return future.get();
}
#endif
ContainerPtr Container::OpenContainerForContentModule(const string& path)
{
	ContainerPtr container = Container::New();
	if (container->Open(path) == false)
		return nullptr;
	return container;
}
Container::PathList Container::PackageLocations() const
{
#if EPUB_COMPILER_SUPPORTS(CXX_INITIALIZER_LISTS)
    XPathWrangler xpath(_ocf, {{"ocf", "urn:oasis:names:tc:opendocument:xmlns:container"}});
#else
    XPathWrangler::NamespaceList __ns;
    __ns["ocf"] = OCFNamespaceURI;
    XPathWrangler xpath(_ocf, __ns);
#endif
	xpath.NameDefaultNamespace("ocf");
    PathList output;
    for ( string& str : xpath.Strings(gRootfilePathsXPath) )
    {
        output.emplace_back(std::move(str));
    }
    
    return output;
}
shared_ptr<Package> Container::DefaultPackage() const
{
    if ( _packages.empty() )
        return nullptr;
    return _packages[0];
}
string Container::Version() const
{
#if EPUB_COMPILER_SUPPORTS(CXX_INITIALIZER_LISTS)
    XPathWrangler xpath(_ocf, {{"ocf", "urn:oasis:names:tc:opendocument:xmlns:container"}});
#else
    XPathWrangler::NamespaceList __ns;
    __ns["ocf"] = OCFNamespaceURI;
    XPathWrangler xpath(_ocf, __ns);
#endif
	xpath.NameDefaultNamespace("ocf");

    std::vector<string> strings = xpath.Strings(gVersionXPath);
    if ( strings.empty() )
        return "1.0";       // guess
    
    return std::move(strings[0]);
}
void Container::LoadEncryption()
{
    unique_ptr<ArchiveReader> pZipReader = _archive->ReaderAtPath(gEncryptionFilePath);
    if ( !pZipReader )
        return;
    
    ArchiveXmlReader reader(std::move(pZipReader));
#if EPUB_USE(LIBXML2)
    shared_ptr<xml::Document> enc = reader.xmlReadDocument(gEncryptionFilePath, nullptr, XML_PARSE_RECOVER|XML_PARSE_NOENT|XML_PARSE_DTDATTR);
#elif EPUB_USE(WIN_XML) || EPUB_USE(WIN_PHONE_XML)
	auto enc = reader.ReadDocument(gEncryptionFilePath, nullptr, 0);
#endif
    if ( !bool(enc) )
        return;
#if EPUB_COMPILER_SUPPORTS(CXX_INITIALIZER_LISTS)
    XPathWrangler xpath(enc, {{"enc", XMLENCNamespaceURI}, {"ocf", OCFNamespaceURI}});
#else
    XPathWrangler::NamespaceList __ns;
    __ns["ocf"] = OCFNamespaceURI;
    __ns["enc"] = XMLENCNamespaceURI;
    XPathWrangler xpath(enc, __ns);
#endif
	xpath.NameDefaultNamespace("ocf");
    xml::NodeSet nodes = xpath.Nodes("/ocf:encryption/enc:EncryptedData");
    if ( nodes.empty() )
    {
		xml::string str(enc->XMLString());
		printf("%s\n", enc->XMLString().utf8());
        return;     // should be a hard error?
    }
    
    for ( auto node : nodes )
    {
        auto encPtr = EncryptionInfo::New(Ptr());
        if ( encPtr->ParseXML(node) )
            _encryption.push_back(encPtr);
    }
}
shared_ptr<EncryptionInfo> Container::EncryptionInfoForPath(const string &path) const
{
    for ( auto item : _encryption )
    {
        if ( item->Path() == path )
            return item;
    }
    
    return nullptr;
}
bool Container::FileExistsAtPath(const string& path) const
{
	return _archive->ContainsItem(path.stl_str());
}
unique_ptr<ByteStream> Container::ReadStreamAtPath(const string &path) const
{
    return _archive->ByteStreamAtPath(path.stl_str());
}

EPUB3_END_NAMESPACE

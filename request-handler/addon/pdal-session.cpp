#include <thread>

#include <node.h>
#include <v8.h>

#include <pdal/PipelineReader.hpp>

#include "pdal-session.hpp"

using namespace v8;

PdalInstance::PdalInstance()
    : m_pipelineManager()
    , m_schema()
    , m_pointBuffer()
{ }

void PdalInstance::initialize(const std::string& pipeline)
{
    std::istringstream ssPipeline(pipeline);
    pdal::PipelineReader pipelineReader(m_pipelineManager);
    pipelineReader.readPipeline(ssPipeline);

    m_pipelineManager.execute();
    const pdal::PointBufferSet& pbSet(m_pipelineManager.buffers());
    m_pointBuffer = pbSet.begin()->get();

    try
    {
        m_schema = packSchema(*m_pipelineManager.schema());

        m_schema.getDimension("X");
        m_schema.getDimension("Y");
        m_schema.getDimension("Z");
    }
    catch (pdal::dimension_not_found&)
    {
        throw std::runtime_error(
                "Pipeline output should contain X, Y and Z dimensions");
    }
}

std::size_t PdalInstance::getNumPoints() const
{
    return m_pointBuffer->getNumPoints();
}

std::string PdalInstance::getSchema() const
{
    return pdal::Schema::to_xml(m_schema);
}

std::size_t PdalInstance::getStride() const
{
    return m_schema.getByteSize();
}

std::size_t PdalInstance::read(
        unsigned char** buffer,
        const std::size_t start,
        const std::size_t count)
{
    *buffer = 0;
    if (start >= getNumPoints())
        throw std::runtime_error("Invalid starting offset in 'read'");

    // If zero points specified, read all points after 'start'.
    const std::size_t pointsToRead(
            count > 0 ?
                std::min<std::size_t>(count, getNumPoints() - start) :
                getNumPoints() - start);

    try
    {
        const pdal::Schema* fullSchema(m_pipelineManager.schema());
        const pdal::schema::index_by_index& idx(
                fullSchema->getDimensions().get<pdal::schema::index>());

        *buffer = new unsigned char[m_schema.getByteSize() * pointsToRead];

        boost::uint8_t* pos(static_cast<boost::uint8_t*>(*buffer));

        for (boost::uint32_t i(start); i < start + pointsToRead; ++i)
        {
            for (boost::uint32_t d = 0; d < idx.size(); ++d)
            {
                if (!idx[d].isIgnored())
                {
                    m_pointBuffer->context().rawPtBuf()->getField(
                            idx[d],
                            i,
                            pos);

                    pos += idx[d].getByteSize();
                }
            }
        }
    }
    catch (...)
    {
        throw std::runtime_error("Failed to read points from PDAL");
    }

    return pointsToRead;
}

pdal::Schema PdalInstance::packSchema(const pdal::Schema& fullSchema)
{
    pdal::Schema packedSchema;

    const pdal::schema::index_by_index& idx(
            fullSchema.getDimensions().get<pdal::schema::index>());

    for (boost::uint32_t d = 0; d < idx.size(); ++d)
    {
        if (!idx[d].isIgnored())
        {
            packedSchema.appendDimension(idx[d]);
        }
    }

    return packedSchema;
}

//////////////////////////////////////////////////////////////////////////////

BufferTransmitter::BufferTransmitter(
        const std::string& host,
        const int port,
        const unsigned char* data,
        const std::size_t size)
    : m_socket()
    , m_data(data)
    , m_size(size)
{
    namespace asio = boost::asio;
    using boost::asio::ip::tcp;

    std::stringstream portStream;
    portStream << port;

    asio::io_service service;
    tcp::resolver resolver(service);

    tcp::resolver::query q(host, portStream.str());
    tcp::resolver::iterator iter = resolver.resolve(q), end;

    m_socket.reset(new tcp::socket(service));

    int retryCount = 0;
    boost::system::error_code ignored_error;

    // Don't fail yet, the setup service may be setting up the receiver.
    tcp::resolver::iterator connectIter;

    while (
        (connectIter = asio::connect(*m_socket, iter, ignored_error)) == end &&
            retryCount++ < 500)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (connectIter == end)
    {
        std::stringstream errStream;
        errStream << "Could not connect to " << host << ":" << port;
        throw std::runtime_error(errStream.str());
    }
}

void BufferTransmitter::transmit()
{
    boost::system::error_code ignored_error;

    boost::asio::write(
            *m_socket,
            boost::asio::buffer(m_data, m_size),
            ignored_error);
}

//////////////////////////////////////////////////////////////////////////////

Persistent<Function> PdalSession::constructor;

PdalSession::PdalSession()
    : m_pdalInstance(new PdalInstance)
{ }

PdalSession::~PdalSession()
{ }

void PdalSession::init(v8::Handle<v8::Object> exports)
{
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(construct);
    tpl->SetClassName(String::NewSymbol("PdalSession"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    // Prototype
    tpl->PrototypeTemplate()->Set(String::NewSymbol("construct"),
        FunctionTemplate::New(create)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("create"),
        FunctionTemplate::New(create)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("destroy"),
        FunctionTemplate::New(destroy)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("getNumPoints"),
        FunctionTemplate::New(getNumPoints)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("getSchema"),
        FunctionTemplate::New(getSchema)->GetFunction());
    tpl->PrototypeTemplate()->Set(String::NewSymbol("read"),
        FunctionTemplate::New(read)->GetFunction());

    constructor = Persistent<Function>::New(tpl->GetFunction());
    exports->Set(String::NewSymbol("PdalSession"), constructor);
}

Handle<Value> PdalSession::construct(const Arguments& args)
{
    HandleScope scope;

    if (args.IsConstructCall())
    {
        // Invoked as constructor with 'new'.
        PdalSession* obj = new PdalSession();
        obj->Wrap(args.This());
        return args.This();
    }
    else
    {
        // Invoked as a function, turn into construct call.
        return scope.Close(constructor->NewInstance());
    }
}

Handle<Value> PdalSession::create(const Arguments& args)
{
    HandleScope scope;

    const std::string pipeline(*v8::String::Utf8Value(args[0]->ToString()));
    Persistent<Function> callback(
            Persistent<Function>::New(Local<Function>::Cast(args[1])));

    PdalSession* obj = ObjectWrap::Unwrap<PdalSession>(args.This());

    // Store everything we'll need to perform the create.
    uv_work_t* req(new uv_work_t);
    req->data = new CreateData(obj->m_pdalInstance, pipeline, callback);

    uv_queue_work(
        uv_default_loop(),
        req,
        (uv_work_cb)([](uv_work_t *req)->void {
            CreateData* createData(reinterpret_cast<CreateData*>(req->data));

            try
            {
                createData->pdalInstance->initialize(createData->pipeline);
            }
            catch (const std::runtime_error& e)
            {
                createData->errMsg = e.what();
            }
            catch (...)
            {
                createData->errMsg = "Unknown error";
            }
        }),
        (uv_after_work_cb)([](uv_work_t* req, int status)->void {
            HandleScope scope;

            CreateData* createData(reinterpret_cast<CreateData*>(req->data));

            // Output args.
            const unsigned argc = 1;
            Local<Value> argv[argc] =
                {
                    Local<Value>::New(String::New(
                            createData->errMsg.data(),
                            createData->errMsg.size()))
                };

            createData->callback->Call(
                Context::GetCurrent()->Global(), argc, argv);

            // Dispose of the persistent handle so the callback may be
            // garbage collected.
            createData->callback.Dispose();

            delete createData;
            delete req;
        }));

    return scope.Close(Undefined());
}

Handle<Value> PdalSession::destroy(const Arguments& args)
{
    HandleScope scope;

    PdalSession* obj = ObjectWrap::Unwrap<PdalSession>(args.This());
    obj->m_pdalInstance.reset();

    return scope.Close(Undefined());
}

Handle<Value> PdalSession::getNumPoints(const Arguments& args)
{
    HandleScope scope;

    PdalSession* obj = ObjectWrap::Unwrap<PdalSession>(args.This());

    return scope.Close(Integer::New(obj->m_pdalInstance->getNumPoints()));
}

Handle<Value> PdalSession::getSchema(const Arguments& args)
{
    HandleScope scope;

    PdalSession* obj = ObjectWrap::Unwrap<PdalSession>(args.This());
    const std::string schema(obj->m_pdalInstance->getSchema());

    return scope.Close(String::New(schema.data(), schema.size()));
}

Handle<Value> PdalSession::read(const Arguments& args)
{
    HandleScope scope;

    if (args[4]->IsUndefined() || !args[4]->IsFunction())
    {
        // Nothing we can do here, no callback supplied.
        return scope.Close(Undefined());
    }

    Persistent<Function> callback(
            Persistent<Function>::New(Local<Function>::Cast(args[4])));

    std::string errMsg("");

    if (args[0]->IsUndefined() || !args[0]->IsString())
        errMsg = "'host' must be a string - args[0]";
    else if (args[1]->IsUndefined() || !args[1]->IsNumber())
        errMsg = "'port' must be a number - args[1]";
    else if (args[2]->IsUndefined() || !args[2]->IsNumber())
        errMsg = "'start' offset must be a number - args[2]";
    else if (args[3]->IsUndefined() || !args[3]->IsNumber())
        errMsg = "'count' must be a number - args[3]";

    if (errMsg.size())
    {
        errorCallback(callback, errMsg);
        return scope.Close(Undefined());
    }

    // Input args.  Type validation is complete by this point.
    const std::string host(*v8::String::Utf8Value(args[0]->ToString()));
    const std::size_t port(args[1]->Uint32Value());
    const std::size_t start(args[2]->Uint32Value());
    const std::size_t count(args[3]->Uint32Value());

    // Provide access to m_pdalInstance from within this static function.
    PdalSession* obj = ObjectWrap::Unwrap<PdalSession>(args.This());

    if (start >= obj->m_pdalInstance->getNumPoints())
    {
        errorCallback(callback, "Invalid start offset in 'read' request");
        return scope.Close(Undefined());
    }
    else
    {
        // Store everything we'll need to perform the read.
        uv_work_t* readReq(new uv_work_t);

        // This structure is the owner of the buffered point data.
        readReq->data = new ReadData(
                obj->m_pdalInstance,
                host,
                port,
                start,
                count,
                callback);

        // Read points asynchronously.
        uv_queue_work(
            uv_default_loop(),
            readReq,
            (uv_work_cb)([](uv_work_t *readReq)->void {
                ReadData* readData(reinterpret_cast<ReadData*>(readReq->data));

                // Buffer the point data from PDAL.
                try
                {
                    // This call will new up the readData->data array.
                    readData->numPoints =
                        readData->pdalInstance->read(
                            &readData->data,
                            readData->start,
                            readData->count);

                    readData->numBytes =
                        readData->numPoints *
                        readData->pdalInstance->getStride();

                    // The BufferTransmitter must not delete readData->data,
                    // and we must take care not to do so until the
                    // BufferTransmitter::transmit() operation is complete.
                    readData->bufferTransmitter.reset(
                        new BufferTransmitter(
                                readData->host,
                                readData->port,
                                readData->data,
                                readData->numBytes));
                }
                catch (const std::runtime_error& e)
                {
                    readData->errMsg = e.what();
                }
                catch (...)
                {
                    readData->errMsg = "Unknown error";
                }
            }),
            (uv_after_work_cb)([](uv_work_t* readReq, int status)->void {
                ReadData* readData(reinterpret_cast<ReadData*>(readReq->data));

                if (readData->errMsg.size())
                {
                    // Propagate the error back to the remote host.
                    errorCallback(readData->callback, readData->errMsg);

                    // Clean up since we won't be calling the async send code.
                    delete [] readData->data;
                    delete readData;
                    return;
                }

                HandleScope scope;

                const unsigned argc = 3;
                Local<Value> argv[argc] =
                    {
                        Local<Value>::New(Null()), // err
                        Local<Value>::New(Integer::New(readData->numPoints)),
                        Local<Value>::New(Integer::New(readData->numBytes))
                    };

                // Call the provided callback to return the status of the
                // data about to be streamed to the remote host.
                readData->callback->Call(
                    Context::GetCurrent()->Global(), argc, argv);

                // Dispose of the persistent handle so this callback may be
                // garbage collected.
                readData->callback.Dispose();

                // Create a token for the actual data transmission portion of
                // the read.
                uv_work_t* sendReq(new uv_work_t);
                sendReq->data = readData;

                // Now stream all the buffered point data to the remote host
                // asynchronously.
                uv_queue_work(
                    uv_default_loop(),
                    sendReq,
                    (uv_work_cb)([](uv_work_t *sendReq)->void {
                        ReadData* readData(
                            reinterpret_cast<ReadData*>(sendReq->data));

                        try
                        {
                            readData->bufferTransmitter->transmit();
                        }
                        catch (...)
                        {
                            std::cout <<
                                "Caught error transmitting buffer" <<
                                std::endl;
                        }
                    }),
                    (uv_after_work_cb)([](uv_work_t* sendReq, int status)->void {
                        ReadData* readData(
                            reinterpret_cast<ReadData*>(sendReq->data));

                        // Read and data transmission complete.  Clean
                        // everything up - readData->bufferTransmitter may
                        // no longer be used.
                        delete [] readData->data;
                        delete readData;
                        delete sendReq;
                    })
                );

                delete readReq;
            })
        );
    }

    return scope.Close(Undefined());
}

void PdalSession::errorCallback(
        Persistent<Function> callback,
        std::string errMsg)
{
    // Don't need a HandleScope here since the caller has already made one.

    const unsigned argc = 1;
    Local<Value> argv[argc] =
        {
            Local<Value>::New(String::New(errMsg.data(), errMsg.size()))
        };

    callback->Call(Context::GetCurrent()->Global(), argc, argv);

    // Dispose of the persistent handle so the callback may be garbage
    // collected.
    callback.Dispose();
}

//////////////////////////////////////////////////////////////////////////////

void init(Handle<Object> exports)
{
    PdalSession::init(exports);
}

NODE_MODULE(pdalSession, init)

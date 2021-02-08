/************************************************************************************
   Copyright (C) 2020, 2021 MariaDB Corporation AB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc.,
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA
*************************************************************************************/


#include "ClientSidePreparedStatement.h"
#include "logger/LoggerFactory.h"
#include "ExceptionFactory.h"
#include "Results.h"
#include "Protocol.h"
#include "util/ClientPrepareResult.h"
#include "parameters/ParameterHolder.h"
#include "ServerSidePreparedStatement.h"
#include "MariaDbParameterMetaData.h"
#include "MariaDbResultSetMetaData.h"
#include "SimpleParameterMetaData.h"

namespace sql
{
namespace mariadb
{
  const Shared::Logger ClientSidePreparedStatement::logger= LoggerFactory::getLogger(typeid(ClientSidePreparedStatement));
 
  /**
    * Private constructor for the clone.
  */
  ClientSidePreparedStatement::ClientSidePreparedStatement(MariaDbConnection* connection,
    int32_t resultSetScrollType,
    int32_t resultSetConcurrency,
    int32_t autoGeneratedKeys,
    Shared::ExceptionFactory& factory)
    : BasePrepareStatement(connection, resultSetScrollType, resultSetConcurrency, autoGeneratedKeys, factory)
  {
  }
  /**
    * Constructor.
    *
    * @param connection connection
    * @param sql sql query
    * @param resultSetScrollType one of the following <code>ResultSet</code> constants: <code>
    *     ResultSet.TYPE_FORWARD_ONLY</code>, <code>ResultSet.TYPE_SCROLL_INSENSITIVE</code>, or
    *     <code>ResultSet.TYPE_SCROLL_SENSITIVE</code>
    * @param resultSetConcurrency a concurrency type; one of <code>ResultSet.CONCUR_READ_ONLY</code>
    *     or <code>ResultSet.CONCUR_UPDATABLE</code>
    * @param autoGeneratedKeys a flag indicating whether auto-generated keys should be returned; one
    *     of <code>Statement.RETURN_GENERATED_KEYS</code> or <code>Statement.NO_GENERATED_KEYS</code>
    * @throws SQLException exception
    */
  ClientSidePreparedStatement::ClientSidePreparedStatement(MariaDbConnection* connection, const SQLString& sql,
    int32_t resultSetScrollType,
    int32_t resultSetConcurrency,
    int32_t autoGeneratedKeys,
    Shared::ExceptionFactory& factory)
    : BasePrepareStatement(connection, resultSetScrollType, resultSetConcurrency, autoGeneratedKeys, factory),
      sqlQuery(sql)
  {
    if (protocol->getOptions()->rewriteBatchedStatements) {
      prepareResult.reset(ClientPrepareResult::rewritableParts(sqlQuery, protocol->noBackslashEscapes()));
    }
    else {
      prepareResult.reset(ClientPrepareResult::parameterParts(sqlQuery, protocol->noBackslashEscapes()));
    }
    parameters.reserve(prepareResult->getParamCount());
    parameters.assign(prepareResult->getParamCount(), Shared::ParameterHolder());
  }

  /**
    * Clone statement.
    *
    * @param connection connection
    * @return Clone statement.
    * @throws CloneNotSupportedException if any error occur.
    */
  ClientSidePreparedStatement* ClientSidePreparedStatement::clone(MariaDbConnection* connection)
  {
    Shared::ExceptionFactory ef(ExceptionFactory::of(this->exceptionFactory->getThreadId(), this->exceptionFactory->getOptions()));
    ClientSidePreparedStatement* clone= new ClientSidePreparedStatement(connection, this->stmt->getResultSetType(), this->stmt->getResultSetConcurrency(),
      this->autoGeneratedKeys, ef);
    clone->sqlQuery= sqlQuery;
    clone->prepareResult= prepareResult;
    clone->parameters.reserve(prepareResult->getParamCount());
    clone->resultSetMetaData= resultSetMetaData;
    clone->parameterMetaData= parameterMetaData;
    return clone;
  }


  bool ClientSidePreparedStatement::executeInternal(int32_t fetchSize)
  {
    // valid parameters
    for (int32_t i= 0; i <prepareResult->getParamCount(); i++) {
      if (!parameters[i]) {
        logger->error("Parameter at position " + std::to_string(i + 1) + " is not set");
        exceptionFactory->raiseStatementError(connection, this)->create("Parameter at position "
          + std::to_string(i + 1) + " is not set", "07004").Throw();
      }
    }

    std::unique_lock<std::mutex> localScopeLock(*protocol->getLock());
    try {
      stmt->executeQueryPrologue(false);
      stmt->setInternalResults(
        new Results(
          this,
          fetchSize,
          false,
          1,
          false,
          stmt->getResultSetType(),
          stmt->getResultSetConcurrency(),
          autoGeneratedKeys,
          protocol->getAutoIncrementIncrement(),
          sqlQuery,
          parameters));
      if (stmt->queryTimeout !=0 && stmt->canUseServerTimeout) {

        protocol->executeQuery(
          protocol->isMasterConnection(), stmt->getInternalResults(), prepareResult.get(), parameters, stmt->queryTimeout);
      }
      else {
        protocol->executeQuery(protocol->isMasterConnection(), stmt->getInternalResults(), prepareResult.get(), parameters);
      }
      stmt->getInternalResults()->commandEnd();
      stmt->executeEpilogue();
      return stmt->getInternalResults()->getResultSet()/*.empty() == true*/;
    }
    catch (SQLException& exception) {
      if (stmt->getInternalResults()/*.empty() == true*/) {
        stmt->getInternalResults()->commandEnd();
      }
      stmt->executeEpilogue();
      localScopeLock.unlock();
      executeExceptionEpilogue(exception).Throw();
    }
    return false;
  }

  /**
    * Adds a set of parameters to this <code>PreparedStatement</code> object's batch of send. <br>
    * <br>
    *
    * @throws SQLException if a database access error occurs or this method is called on a closed
    *     <code>PreparedStatement</code>
    * @see Statement#addBatch
    * @since 1.2
    */
  void ClientSidePreparedStatement::addBatch()
  {
    std::vector<Shared::ParameterHolder> holder(prepareResult->getParamCount());
    for (int32_t i= 0; i <holder.size(); i++) {
      holder[i]= parameters[i];
      if (!holder[i]) {
        logger->error(
          "You need to set exactly "
          + std::to_string(prepareResult->getParamCount())
          + " parameters on the prepared statement");
        exceptionFactory->raiseStatementError(connection, this)->create(
          "You need to set exactly "
          + std::to_string(prepareResult->getParamCount())
          + " parameters on the prepared statement").Throw();
      }
    }
    parameterList.push_back(holder);
  }


  void ClientSidePreparedStatement::clearBatch()
  {
    parameterList.clear();
    hasLongData= false;
    this->parameters.clear(); // clear() doesn't change capacity
  }

  /** {inheritdoc}. */
  Ints& ClientSidePreparedStatement::executeBatch()
  {
    stmt->checkClose();
    std::size_t size= parameterList.size();
    if (size == 0) {
      return stmt->batchRes.wrap(nullptr, 0);
    }

    std::lock_guard<std::mutex> localScopeLock(*protocol->getLock());
    try {
      executeInternalBatch(size);
      stmt->getInternalResults()->commandEnd();
      return stmt->batchRes.wrap(stmt->getInternalResults()->getCmdInformation()->getUpdateCounts());

    }
    catch (SQLException& sqle) {
      stmt->executeBatchEpilogue();
      throw stmt->executeBatchExceptionEpilogue(sqle, size);
    }
    stmt->executeBatchEpilogue();
  }

  /**
    * Non JDBC : Permit to retrieve server update counts when using option rewriteBatchedStatements.
    *
    * @return an array of update counts containing one element for each command in the batch. The
    *     elements of the array are ordered according to the order in which commands were added to
    *     the batch.
    */
  sql::Ints& ClientSidePreparedStatement::getServerUpdateCounts()
  {
    if (stmt->getInternalResults() && stmt->getInternalResults()->getCmdInformation()) {
      return stmt->batchRes.wrap(stmt->getInternalResults()->getCmdInformation()->getServerUpdateCounts());
    }
    return stmt->batchRes.wrap(nullptr, 0);
  }

  /**
    * Execute batch, like executeBatch(), with returning results with long[]. For when row count may
    * exceed Integer.MAX_VALUE.
    *
    * @return an array of update counts (one element for each command in the batch)
    * @throws SQLException if a database error occur.
    */
  sql::Longs& ClientSidePreparedStatement::executeLargeBatch()
  {
    stmt->checkClose();
    std::size_t size= parameterList.size();
    if (size == 0) {
      return stmt->largeBatchRes.wrap(nullptr, 0);
    }

    std::lock_guard<std::mutex> localScopeLock(*protocol->getLock());
    try {
      executeInternalBatch(size);
      stmt->getInternalResults()->commandEnd();
      return stmt->largeBatchRes.wrap(stmt->getInternalResults()->getCmdInformation()->getLargeUpdateCounts());
    }
    catch (SQLException& sqle) {
      stmt->executeBatchEpilogue();
      throw stmt->executeBatchExceptionEpilogue(sqle, size);
    }
    stmt->executeBatchEpilogue();
  }

  /**
    * Choose better way to execute queries according to query and options.
    *
    * @param size parameters number
    * @throws SQLException if any error occur
    */
  void ClientSidePreparedStatement::executeInternalBatch(std::size_t size)
  {
    std::vector<Shared::ParameterHolder> dummy;

    stmt->executeQueryPrologue(true);
    stmt->setInternalResults(
      new Results(
        this,
        0,
        true,
        size,
        false,
        stmt->getResultSetType(),
        stmt->getResultSetConcurrency(),
        autoGeneratedKeys,
        protocol->getAutoIncrementIncrement(),
        nullptr,
        dummy));
    if (protocol->executeBatchClient(
      protocol->isMasterConnection(), stmt->getInternalResults(), prepareResult.get(), parameterList, hasLongData)) {
      return;
    }

    // send query one by one, reading results for each query before sending another one
    SQLException exception("");

    if (stmt->queryTimeout > 0) {
      for (auto& it: parameterList) {
        protocol->stopIfInterrupted();
        try {
          protocol->executeQuery(
            protocol->isMasterConnection(),
            stmt->getInternalResults(),
            prepareResult.get(),
            it);
        }
        catch (SQLException& e) {
          if (stmt->options->continueBatchOnError) {
            exception= e;
          }
          else {
            throw e;
          }
        }
      }
    }
    else {
      for (auto& it : parameterList) {
        try {
          protocol->executeQuery(
            protocol->isMasterConnection(),
            stmt->getInternalResults(),
            prepareResult.get(),
            it);
        }
        catch (SQLException& e) {
          if (stmt->options->continueBatchOnError) {
            exception= e;
          }
          else {
            throw e;
          }
        }
      }
    }
    /* We creating default exception w/out message.
       Using that to test if we caught an exception during the execution */
    if (*exception.getMessage() != '\0') {
      throw exception;
    }
  }

  /**
    * Retrieves a <code>ResultSetMetaData</code> object that contains information about the columns
    * of the <code>ResultSet</code> object that will be returned when this <code>PreparedStatement
    * </code> object is executed. <br>
    * Because a <code>PreparedStatement</code> object is precompiled, it is possible to know about
    * the <code>ResultSet</code> object that it will return without having to execute it.
    * Consequently, it is possible to invoke the method <code>getMetaData</code> on a <code>
    * PreparedStatement</code> object rather than waiting to execute it and then invoking the <code>
    * ResultSet.getMetaData</code> method on the <code>ResultSet</code> object that is returned.
    *
    * @return the description of a <code>ResultSet</code> object's columns or <code>null</code> if
    *     the driver cannot return a <code>ResultSetMetaData</code> object
    * @throws SQLException if a database access error occurs or this method is called on a closed
    *     <code>PreparedStatement</code>
    */
  sql::ResultSetMetaData* ClientSidePreparedStatement::getMetaData()
  {
    stmt->checkClose();
    ResultSet* rs= getResultSet();
    if (rs != nullptr) {
      return rs->getMetaData();
    }
    if (!resultSetMetaData) {
      loadParametersData();
    }
    return resultSetMetaData.get();
  }

  /**
    * Set parameter.
    *
    * @param parameterIndex index
    * @param holder parameter holder
    * @throws SQLException if index position doesn't correspond to query parameters
    */
  void ClientSidePreparedStatement::setParameter(int32_t parameterIndex, ParameterHolder* holder)
  {
    if (parameterIndex >= 1 && parameterIndex < prepareResult->getParamCount() + 1) {
      parameters[parameterIndex - 1].reset(holder);
    }
    else {
      SQLString error("Could not set parameter at position "
        + std::to_string(parameterIndex)
        + " (values was "
        + holder->toString()
        + ")\n"
        + "Query - conn:"
        + std::to_string(protocol->getServerThreadId())
        + "("
        + (protocol->isMasterConnection() ? "M" : "S")
        + ") ");

      delete holder;

      if (stmt->options->maxQuerySizeToLog > 0) {
        error.append(" - \"");
        if (sqlQuery.size() < stmt->options->maxQuerySizeToLog) {
          error.append(sqlQuery);
        }
        else {
          error.append(sqlQuery.substr(0, stmt->options->maxQuerySizeToLog) + "...");
        }
        error.append("\"");
      }
      else {
        error.append(" - \""+sqlQuery +"\"");
      }

      logger->error(error);
      exceptionFactory->raiseStatementError(connection, this)->create(error).Throw();
    }
  }

  /**
    * Retrieves the number, types and properties of this <code>PreparedStatement</code> object's
    * parameters.
    *
    * @return a <code>ParameterMetaData</code> object that contains information about the number,
    *     types and properties for each parameter marker of this <code>PreparedStatement</code>
    *     object
    * @throws SQLException if a database access error occurs or this method is called on a closed
    *     <code>PreparedStatement</code>
    * @see ParameterMetaData
    * @since 1.4
    */
  ParameterMetaData* ClientSidePreparedStatement::getParameterMetaData()
  {
    stmt->checkClose();
    if (!parameterMetaData) {
      loadParametersData();
    }
    return parameterMetaData.get();
  }

  void ClientSidePreparedStatement::loadParametersData()
  {
    try {
      ServerSidePreparedStatement ssps(
        connection,
        sqlQuery,
        ResultSet::TYPE_SCROLL_INSENSITIVE,
        ResultSet::CONCUR_READ_ONLY,
        Statement::NO_GENERATED_KEYS,
        exceptionFactory);
      resultSetMetaData.reset(ssps.getMetaData());
      parameterMetaData.reset(ssps.getParameterMetaData());
    }
    catch (SQLException&) {
      parameterMetaData.reset(new SimpleParameterMetaData(static_cast<uint32_t>(prepareResult->getParamCount())));
    }
  }

  /**
    * Clears the current parameter values immediately.
    *
    * <p>In general, parameter values remain in force for repeated use of a statement. Setting a
    * parameter value automatically clears its previous value. However, in some cases it is useful to
    * immediately release the resources used by the current parameter values; this can be done by
    * calling the method <code>clearParameters</code>.
    */
  void ClientSidePreparedStatement::clearParameters()
  {
    parameters.clear();
    parameters.assign(prepareResult->getParamCount(), Shared::ParameterHolder());
  }

  void ClientSidePreparedStatement::close()
  {
    stmt->close();
    connection= nullptr;
  }

  uint32_t ClientSidePreparedStatement::getParameterCount()
  {
    return static_cast<uint32_t>(prepareResult->getParamCount());
  }

  SQLString ClientSidePreparedStatement::toString()
  {
    SQLString sb("sql : '"+sqlQuery +"'");
    sb.append(", parameters : [");
    for (const auto cit:parameters ) {
      if (!cit) {
        sb.append("NULL");
      }
      else {
        sb.append(cit->toString());
      }
      if (cit != parameters.back()) {
        sb.append(",");
      }
    }
    sb.append("]");
    return sb;
  }

  ClientPrepareResult* ClientSidePreparedStatement::getPrepareResult()
  {
    return prepareResult.get();
  }
}
}

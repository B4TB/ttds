{-# LANGUAGE ForeignFunctionInterface #-}
{-# LANGUAGE LambdaCase #-}
{-# LANGUAGE OverloadedStrings #-}

module Main where

import Auth (TokenStore, checkAuth, initTokenStore, register, unregister, verifyAdmin)
import Control.Monad.IO.Class (liftIO)
import Data.Text (unpack)
import Data.Text.Lazy (pack, toStrict)
import Data.UUID (toText)
import GHC.Conc (atomically)
import Network.HTTP.Types.Status (badRequest400, unauthorized401)
import Proc (Proc, call, kill, launch, mkCommand)
import System.Environment (getArgs)
import System.Posix.Signals (Handler (..), installHandler, sigINT)
import Web.Scotty (ScottyM, ActionM, capture, delete, finish, header, notFound, pathParam, post, queryParam, scotty, status, text)

foreign import ccall "reallyExit" reallyExit :: IO ()

main :: IO ()
main = getArgs >>= launch >>= setupAndRun
  where
    setupAndRun proc = setupTerm proc >> runWithProc proc
    runWithProc proc = initTokenStore >>= runWebServer proc

    setupTerm proc = installHandler sigINT (sigintHandler proc) Nothing
    sigintHandler proc =
      Catch $
        putStrLn "Exiting." >> kill proc >> reallyExit

runWebServer :: Proc -> TokenStore -> IO ()
runWebServer proc ts =
  scotty 8080 $ do
    post "/raw/:text" $ requireAdmin ts >> pathParam "text" >>= routeRaw . mkCommand
    post "/pane/:pane/create" $ do
      pane <- pathParam "pane"
      color <- queryParam "color"
      liftIO $ putStrLn color
      callCreate pane color
      registerPane pane

    makeDrawRoute "RECT" "rect" ["color", "x", "y", "w", "h"]
    makeDrawRoute "CIRCLE" "circle" ["color", "x", "y", "r"]
    makeDrawRoute "LINE" "line" ["color", "x", "y", "x2", "y2"]
    makeDrawRoute "COPY_RECT" "copy_rect" ["x2", "y2", "x", "y", "w", "h"]

    delete "/pane/:pane" $
      pathParam "pane" >>= checkAuthScotty >>= \pane ->
        callDelete pane >> liftIO (unregister ts pane)

    notFound $ text "404\n"
  where
    makeDrawRoute :: String -> String -> [String] -> ScottyM ()
    makeDrawRoute cmd slug args = post (capture $ "/pane/:pane/" ++ slug) $ do
      pane <- pathParam "pane"
      vals <- mapM (queryParam . pack) args
      _ <- checkAuthScotty pane
      callStr $ unpack pane ++ ": " ++ cmd ++ " " ++ unwords vals

    callAct cmd = liftIO $ call proc cmd
    routeRaw cmd = callAct cmd >>= text . pack

    registerPane name =
      (liftIO . register ts) name >>= \case
        Just uuid -> (text . pack . unpack . toText) uuid
        Nothing -> status badRequest400 >> text "Pane with same name exists." >> finish

    checkAuthScotty name = header "Auth" >>= check >>= serve
      where
        check (Just uuid) = liftIO $ checkAuth ts name uuid
        check Nothing = status unauthorized401 >> finish

        serve True = return name
        serve False = status unauthorized401 >> finish

    callCreate name color = callStr $ unpack name ++ ": CREATE " ++ color
    callDelete name = callStr $ unpack name ++ ": REMOVE"

    callStr cmd = (liftIO . callAct . mkCommand) cmd >>= \case
      "OK" -> return ()
      x -> (text . pack) x >> finish

requireAdmin :: TokenStore -> ActionM ()
requireAdmin ts = header "Auth" >>= isOk >>= verify
  where
    isOk (Just token) = liftIO $ atomically $ verifyAdmin ts $ toStrict token
    isOk Nothing = return False

    verify True = return ()
    verify False = status unauthorized401 >> finish
